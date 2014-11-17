#ifndef XGBOOST_TREE_UPDATER_BASEMAKER_INL_HPP_
#define XGBOOST_TREE_UPDATER_BASEMAKER_INL_HPP_
/*!
 * \file updater_basemaker-inl.hpp
 * \brief implement a common tree constructor
 * \author Tianqi Chen
 */
#include <vector>
#include <algorithm>
#include "../utils/random.h"

namespace xgboost {
namespace tree {
/*! 
 * \brief base tree maker class that defines common operation
 *  needed in tree making
 */
class BaseMaker: public IUpdater {
 public:
  // destructor
  virtual ~BaseMaker(void) {}
  // set training parameter
  virtual void SetParam(const char *name, const char *val) {
    param.SetParam(name, val);
  }
  
 protected:
  // ------static helper functions ------
  // helper function to get to next level of the tree
  /*! \brief this is  helper function for row based data*/
  inline static int NextLevel(const RowBatch::Inst &inst, const RegTree &tree, int nid) {
    const RegTree::Node &n = tree[nid];
    bst_uint findex = n.split_index();
    for (unsigned i = 0; i < inst.length; ++i) {
      if (findex == inst[i].index) {
        if (inst[i].fvalue < n.split_cond()) {
          return n.cleft();
        } else {
          return n.cright();
        }
      }
    }
    return n.cdefault();
  }
  /*! \brief get number of omp thread in current context */
  inline static int get_nthread(void) {
    int nthread;
    #pragma omp parallel
    {
      nthread = omp_get_num_threads();
    }
    return nthread;
  }
  // ------class member helpers---------
  /*! \brief initialize temp data structure */
  inline void InitData(const std::vector<bst_gpair> &gpair,
                       const IFMatrix &fmat,
                       const std::vector<unsigned> &root_index,
                       const RegTree &tree) {
    utils::Assert(tree.param.num_nodes == tree.param.num_roots,
                  "TreeMaker: can only grow new tree");
    {// setup position
      position.resize(gpair.size());
      if (root_index.size() == 0) {
        std::fill(position.begin(), position.end(), 0);
      } else {
        for (size_t i = 0; i < position.size(); ++i) {
          position[i] = root_index[i];
          utils::Assert(root_index[i] < (unsigned)tree.param.num_roots,
                        "root index exceed setting");
        }
      }
      // mark delete for the deleted datas
      for (size_t i = 0; i < position.size(); ++i) {
        if (gpair[i].hess < 0.0f) position[i] = ~position[i];
      }
      // mark subsample
      if (param.subsample < 1.0f) {
        for (size_t i = 0; i < position.size(); ++i) {
          if (gpair[i].hess < 0.0f) continue;
          if (random::SampleBinary(param.subsample) == 0) position[i] = ~position[i];
        }
      }
    }
    {// expand query
      qexpand.reserve(256); qexpand.clear();
      for (int i = 0; i < tree.param.num_roots; ++i) {
        qexpand.push_back(i);
      }
      this->UpdateNode2WorkIndex(tree);
    }
  }
  /*! \brief update queue expand add in new leaves */
  inline void UpdateQueueExpand(const RegTree &tree) {
    std::vector<int> newnodes;
    for (size_t i = 0; i < qexpand.size(); ++i) {
      const int nid = qexpand[i];
      if (!tree[nid].is_leaf()) {
        newnodes.push_back(tree[nid].cleft());
        newnodes.push_back(tree[nid].cright());
      }
    }
    // use new nodes for qexpand
    qexpand = newnodes;
    this->UpdateNode2WorkIndex(tree);
  }
  // return decoded position
  inline int DecodePosition(bst_uint ridx) const{
    const int pid = position[ridx];
    return pid < 0 ? ~pid : pid;
  }
  // encode the encoded position value for ridx
  inline void SetEncodePosition(bst_uint ridx, int nid) {
    if (position[ridx] < 0) {
      position[ridx] = ~nid;
    } else {
      position[ridx] = nid;
    }
  }
  /*! 
   * \brief this is helper function uses column based data structure,
   *        reset the positions to the lastest one
   * \param nodes the set of nodes that contains the split to be used
   * \param p_fmat feature matrix needed for tree construction
   * \param tree the regression tree structure
   */
  inline void ResetPositionCol(const std::vector<int> &nodes, IFMatrix *p_fmat, const RegTree &tree) {
    // set the positions in the nondefault
    this->SetNonDefaultPositionCol(nodes, p_fmat, tree);
    // set rest of instances to default position
    const std::vector<bst_uint> &rowset = p_fmat->buffered_rowset();
    // set default direct nodes to default
    // for leaf nodes that are not fresh, mark then to ~nid, 
    // so that they are ignored in future statistics collection
    const bst_omp_uint ndata = static_cast<bst_omp_uint>(rowset.size());
    
    #pragma omp parallel for schedule(static)
    for (bst_omp_uint i = 0; i < ndata; ++i) {
      const bst_uint ridx = rowset[i];
      const int nid = this->DecodePosition(ridx);
      if (tree[nid].is_leaf()) {
        // mark finish when it is not a fresh leaf
        if (tree[nid].cright() == -1) {
          position[ridx] = ~nid;
        }
        } else {
        // push to default branch
        if (tree[nid].default_left()) {
          this->SetEncodePosition(ridx, tree[nid].cleft());
        } else {
          this->SetEncodePosition(ridx, tree[nid].cright());
        }
      }
    }
  }
  /*!
   * \brief this is helper function uses column based data structure,
   *        update all positions into nondefault branch, if any, ignore the default branch
   * \param nodes the set of nodes that contains the split to be used
   * \param p_fmat feature matrix needed for tree construction
   * \param tree the regression tree structure
   */
  virtual void SetNonDefaultPositionCol(const std::vector<int> &nodes,
                                        IFMatrix *p_fmat, const RegTree &tree) {
    // step 1, classify the non-default data into right places
    std::vector<unsigned> fsplits;
    for (size_t i = 0; i < nodes.size(); ++i) {
      const int nid = nodes[i];
      if (!tree[nid].is_leaf()) {
        fsplits.push_back(tree[nid].split_index());
      }
    }
    std::sort(fsplits.begin(), fsplits.end());
    fsplits.resize(std::unique(fsplits.begin(), fsplits.end()) - fsplits.begin());
    
    utils::IIterator<ColBatch> *iter = p_fmat->ColIterator(fsplits);
    while (iter->Next()) {
      const ColBatch &batch = iter->Value();
      for (size_t i = 0; i < batch.size; ++i) {
        ColBatch::Inst col = batch[i];
        const bst_uint fid = batch.col_index[i];
        const bst_omp_uint ndata = static_cast<bst_omp_uint>(col.length);
        #pragma omp parallel for schedule(static)
        for (bst_omp_uint j = 0; j < ndata; ++j) {
          const bst_uint ridx = col[j].index;
          const float fvalue = col[j].fvalue;
          const int nid = this->DecodePosition(ridx);
          // go back to parent, correct those who are not default
          if (!tree[nid].is_leaf() && tree[nid].split_index() == fid) {
            if(fvalue < tree[nid].split_cond()) {
              this->SetEncodePosition(ridx, tree[nid].cleft());
            } else {
              this->SetEncodePosition(ridx, tree[nid].cright());
            }
          }
        }
      }
    }
  }
  /*! \brief training parameter of tree grower */
  TrainParam param;
  /*! \brief queue of nodes to be expanded */
  std::vector<int> qexpand;
  /*!
   * \brief map active node to is working index offset in qexpand,
   *   can be -1, which means the node is node actively expanding
   */
  std::vector<int> node2workindex;
  /*!
   * \brief position of each instance in the tree
   *   can be negative, which means this position is no longer expanding
   *   see also Decode/EncodePosition
   */
  std::vector<int> position;

 private:
  inline void UpdateNode2WorkIndex(const RegTree &tree) {
    // update the node2workindex
    std::fill(node2workindex.begin(), node2workindex.end(), -1);
    node2workindex.resize(tree.param.num_nodes);
    for (size_t i = 0; i < qexpand.size(); ++i) {
      node2workindex[qexpand[i]] = static_cast<int>(i);
    }
  }
};
}  // namespace tree
}  // namespace xgboost
#endif // XGBOOST_TREE_UPDATER_BASEMAKER_INL_HPP_
