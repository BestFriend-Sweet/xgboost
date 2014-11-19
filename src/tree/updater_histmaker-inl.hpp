#ifndef XGBOOST_TREE_UPDATER_HISTMAKER_INL_HPP_
#define XGBOOST_TREE_UPDATER_HISTMAKER_INL_HPP_
/*!
 * \file updater_histmaker-inl.hpp
 * \brief use histogram counting to construct a tree
 * \author Tianqi Chen
 */
#include <vector>
#include <algorithm>
#include "../sync/sync.h"
#include "../utils/quantile.h"
#include "../utils/group_data.h"
#include "./updater_basemaker-inl.hpp"

namespace xgboost {
namespace tree {
template<typename TStats>
class HistMaker: public BaseMaker {
 public:
  virtual ~HistMaker(void) {}
  virtual void Update(const std::vector<bst_gpair> &gpair,
                      IFMatrix *p_fmat,
                      const BoosterInfo &info,
                      const std::vector<RegTree*> &trees) {
    TStats::CheckInfo(info);
    // rescale learning rate according to size of trees
    float lr = param.learning_rate;
    param.learning_rate = lr / trees.size();
    // build tree
    for (size_t i = 0; i < trees.size(); ++i) {
      this->Update(gpair, p_fmat, info, trees[i]);
    }
    param.learning_rate = lr;
  }

 protected:
  /*! \brief a single histogram */
  struct HistUnit {
    /*! \brief cutting point of histogram, contains maximum point */
    const bst_float *cut;
    /*! \brief content of statistics data */    
    TStats *data;
    /*! \brief size of histogram */
    unsigned size;
    // default constructor
    HistUnit(void) {}
    // constructor
    HistUnit(const bst_float *cut, TStats *data, unsigned size)
        : cut(cut), data(data), size(size) {}
    /*! \brief add a histogram to data */
    inline void Add(bst_float fv, 
                    const std::vector<bst_gpair> &gpair,
                    const BoosterInfo &info,
                    const bst_uint ridx) {
      unsigned i = std::upper_bound(cut, cut + size, fv) - cut;
      utils::Assert(size != 0, "try insert into size=0");
      utils::Assert(i < size, 
                    "maximum value must be in cut, fv = %g, cutmax=%g", fv, cut[size-1]);
      data[i].Add(gpair, info, ridx);
    }
  };
  /*! \brief a set of histograms from different index */
  struct HistSet {
    /*! \brief the index pointer of each histunit */
    const unsigned *rptr;
    /*! \brief cutting points in each histunit */
    const bst_float *cut;
    /*! \brief data in different hist unit */
    std::vector<TStats> data;
    /*! \brief */
    inline HistUnit operator[](bst_uint fid) {
      return HistUnit(cut + rptr[fid],
                      &data[0] + rptr[fid],
                      rptr[fid+1] - rptr[fid]);
    }
  };
  // thread workspace 
  struct ThreadWSpace {
    /*! \brief actual unit pointer */
    std::vector<unsigned> rptr;
    /*! \brief cut field */
    std::vector<bst_float> cut;
    // per thread histset
    std::vector<HistSet> hset;
    // initialize the hist set
    inline void Init(const TrainParam &param, int nthread) {
      hset.resize(nthread);
      // cleanup statistics
      for (int tid = 0; tid < nthread; ++tid) {
        for (size_t i = 0; i < hset[tid].data.size(); ++i) {
          hset[tid].data[i].Clear();
        }
        hset[tid].rptr = BeginPtr(rptr);
        hset[tid].cut = BeginPtr(cut);
        hset[tid].data.resize(cut.size(), TStats(param));        
      }
    }
    // aggregate all statistics to hset[0]
    inline void Aggregate(void) {
      bst_omp_uint nsize = static_cast<bst_omp_uint>(cut.size());
      #pragma omp parallel for schedule(static)
      for (bst_omp_uint i = 0; i < nsize; ++i) {
        for (size_t tid = 1; tid < hset.size(); ++tid) {
          hset[0].data[i].Add(hset[tid].data[i]);
        }
      }
    }
    /*! \brief clear the workspace */
    inline void Clear(void) {
      cut.clear(); rptr.resize(1); rptr[0] = 0;
    }
    /*! \brief total size */
    inline size_t Size(void) const {
      return rptr.size() - 1;
    }
  };
  // workspace of thread
  ThreadWSpace wspace;
  // reducer for histogram
  sync::Reducer<TStats> histred;
  // update function implementation
  virtual void Update(const std::vector<bst_gpair> &gpair,
                      IFMatrix *p_fmat,
                      const BoosterInfo &info,
                      RegTree *p_tree) {
    this->InitData(gpair, *p_fmat, info.root_index, *p_tree);
    for (int depth = 0; depth < param.max_depth; ++depth) {
      // reset and propose candidate split
      this->ResetPosAndPropose(gpair, p_fmat, info, *p_tree);
      // create histogram
      this->CreateHist(gpair, p_fmat, info, *p_tree);
      // find split based on histogram statistics
      this->FindSplit(depth, gpair, p_fmat, info, p_tree);
      // reset position after split
      this->ResetPositionAfterSplit(p_fmat, *p_tree);
      this->UpdateQueueExpand(*p_tree);
      // if nothing left to be expand, break
      if (qexpand.size() == 0) break;
    }
    for (size_t i = 0; i < qexpand.size(); ++i) {
      const int nid = qexpand[i];
      (*p_tree)[nid].set_leaf(p_tree->stat(nid).base_weight * param.learning_rate);
    }
  }
  // this function does two jobs
  // (1) reset the position in array position, to be the latest leaf id
  // (2) propose a set of candidate cuts and set wspace.rptr wspace.cut correctly 
  virtual void ResetPosAndPropose(const std::vector<bst_gpair> &gpair,
                                  IFMatrix *p_fmat,
                                  const BoosterInfo &info,
                                  const RegTree &tree)  = 0;  
  // reset position after split, this is not a must, depending on implementation
  virtual void ResetPositionAfterSplit(IFMatrix *p_fmat,
                                       const RegTree &tree) {
  }
  virtual void CreateHist(const std::vector<bst_gpair> &gpair,
                          IFMatrix *p_fmat,
                          const BoosterInfo &info,
                          const RegTree &tree) {
    bst_uint num_feature = tree.param.num_feature;
    // intialize work space
    wspace.Init(param, this->get_nthread());
    // start accumulating statistics
    utils::IIterator<RowBatch> *iter = p_fmat->RowIterator();
    iter->BeforeFirst();
    while (iter->Next()) {
      const RowBatch &batch = iter->Value();
      utils::Check(batch.size < std::numeric_limits<unsigned>::max(),
                   "too large batch size ");
      const bst_omp_uint nbatch = static_cast<bst_omp_uint>(batch.size);
      #pragma omp parallel for schedule(static)
      for (bst_omp_uint i = 0; i < nbatch; ++i) {
        RowBatch::Inst inst = batch[i];
        const int tid = omp_get_thread_num();
        HistSet &hset = wspace.hset[tid];
        const bst_uint ridx = static_cast<bst_uint>(batch.base_rowid + i);
        int nid = position[ridx];
        if (nid >= 0) {
          const int wid = this->node2workindex[nid];
          for (bst_uint i = 0; i < inst.length; ++i) {
            utils::Assert(inst[i].index < num_feature, "feature index exceed bound");
            // feature histogram
            hset[inst[i].index + wid * (num_feature+1)]
                .Add(inst[i].fvalue, gpair, info, ridx);
          }
          // node histogram, use num_feature to borrow space
          hset[num_feature + wid * (num_feature + 1)]
              .data[0].Add(gpair, info, ridx);
        }
      }
    }
    // accumulating statistics together
    wspace.Aggregate();
    // sync the histogram
    histred.AllReduce(BeginPtr(wspace.hset[0].data), wspace.hset[0].data.size());
  }

 private:
  inline void EnumerateSplit(const HistUnit &hist, 
                             const TStats &node_sum,
                             bst_uint fid,
                             SplitEntry *best,
                             TStats *left_sum) {
    if (hist.size == 0) return;

    double root_gain = node_sum.CalcGain(param);
    TStats s(param), c(param);
    for (bst_uint i = 0; i < hist.size; ++i) {
      s.Add(hist.data[i]);
      if (s.sum_hess >= param.min_child_weight) {
        c.SetSubstract(node_sum, s);
        if (c.sum_hess >= param.min_child_weight) {
          double loss_chg = s.CalcGain(param) + c.CalcGain(param) - root_gain;
          if (best->Update(loss_chg, fid, hist.cut[i], false)) {
            *left_sum = s;
          }
        }
      }
    }
    s.Clear();
    for (bst_uint i = hist.size - 1; i != 0; --i) {
      s.Add(hist.data[i]);
      if (s.sum_hess >= param.min_child_weight) {
        c.SetSubstract(node_sum, s);
        if (c.sum_hess >= param.min_child_weight) {
          double loss_chg = s.CalcGain(param) + c.CalcGain(param) - root_gain;
          if (best->Update(loss_chg, fid, hist.cut[i-1], true)) {
            *left_sum = c;
          }
        }
      }
    }
  }
  inline void FindSplit(int depth,
                        const std::vector<bst_gpair> &gpair,
                        IFMatrix *p_fmat,
                        const BoosterInfo &info,
                        RegTree *p_tree) {
    const bst_uint num_feature = p_tree->param.num_feature;
    // get the best split condition for each node
    std::vector<SplitEntry> sol(qexpand.size());
    std::vector<TStats> left_sum(qexpand.size());    
    bst_omp_uint nexpand = static_cast<bst_omp_uint>(qexpand.size());
    #pragma omp parallel for schedule(dynamic, 1)
    for (bst_omp_uint wid = 0; wid < nexpand; ++ wid) {
      const int nid = qexpand[wid];
      utils::Assert(node2workindex[nid] == static_cast<int>(wid),
                    "node2workindex inconsistent");
      SplitEntry &best = sol[wid];
      TStats &node_sum = wspace.hset[0][num_feature + wid * (num_feature + 1)].data[0];
      for (bst_uint fid = 0; fid < num_feature; ++ fid) {
        EnumerateSplit(this->wspace.hset[0][fid + wid * (num_feature+1)],
                       node_sum, fid, &best, &left_sum[wid]);
      }
    }
    // get the best result, we can synchronize the solution
    for (bst_omp_uint wid = 0; wid < nexpand; ++ wid) {
      const int nid = qexpand[wid];
      const SplitEntry &best = sol[wid];
      const TStats &node_sum = wspace.hset[0][num_feature + wid * (num_feature + 1)].data[0];
      this->SetStats(p_tree, nid, node_sum);
      // set up the values
      p_tree->stat(nid).loss_chg = best.loss_chg;
      // now we know the solution in snode[nid], set split
      if (best.loss_chg > rt_eps) {
        p_tree->AddChilds(nid);
        (*p_tree)[nid].set_split(best.split_index(),
                                 best.split_value, best.default_left());
        // mark right child as 0, to indicate fresh leaf
        (*p_tree)[(*p_tree)[nid].cleft()].set_leaf(0.0f, 0);        
        (*p_tree)[(*p_tree)[nid].cright()].set_leaf(0.0f, 0);
        // right side sum
        TStats right_sum;
        right_sum.SetSubstract(node_sum, left_sum[wid]);
        this->SetStats(p_tree, (*p_tree)[nid].cleft(), left_sum[wid]);
        this->SetStats(p_tree, (*p_tree)[nid].cright(), right_sum);
      } else {
        (*p_tree)[nid].set_leaf(p_tree->stat(nid).base_weight * param.learning_rate);
      }
    }
  }
  
  inline void SetStats(RegTree *p_tree, int nid, const TStats &node_sum) {
    p_tree->stat(nid).base_weight = node_sum.CalcWeight(param);
    p_tree->stat(nid).sum_hess = static_cast<float>(node_sum.sum_hess);
    node_sum.SetLeafVec(param, p_tree->leafvec(nid));    
  }
};

template<typename TStats>
class CQHistMaker: public HistMaker<TStats> {
 protected:
  struct HistEntry {
    typename HistMaker<TStats>::HistUnit hist;
    unsigned istart;
    /*! 
     * \brief add a histogram to data,
     * do linear scan, start from istart
     */
    inline void Add(bst_float fv,
                    const std::vector<bst_gpair> &gpair,
                    const BoosterInfo &info,
                    const bst_uint ridx) {
      while (istart < hist.size && !(fv < hist.cut[istart])) ++istart;
      utils::Assert(istart != hist.size, "the bound variable must be max");
      hist.data[istart].Add(gpair, info, ridx);
    }
  };
  typedef utils::WXQuantileSketch<bst_float, bst_float> WXQSketch;
  virtual void CreateHist(const std::vector<bst_gpair> &gpair,
                          IFMatrix *p_fmat,
                          const BoosterInfo &info,
                          const RegTree &tree) {
    this->wspace.Init(this->param, 1);
    thread_hist.resize(this->get_nthread());
    // start accumulating statistics
    utils::IIterator<ColBatch> *iter = p_fmat->ColIterator();
    iter->BeforeFirst();
    while (iter->Next()) {
      const ColBatch &batch = iter->Value();
      // start enumeration
      const bst_omp_uint nsize = static_cast<bst_omp_uint>(batch.size);
      #pragma omp parallel for schedule(dynamic, 1)
      for (bst_omp_uint i = 0; i < nsize; ++i) {
        this->UpdateHistCol(gpair, batch[i], info, tree,
                            batch.col_index[i],                        
                            &thread_hist[omp_get_thread_num()]);       
      }
    }
    for (size_t i = 0; i < this->qexpand.size(); ++i) {
      const int nid = this->qexpand[i];
      const int wid = this->node2workindex[nid];
      this->wspace.hset[0][tree.param.num_feature + wid * (tree.param.num_feature+1)]
          .data[0] = node_stats[nid];
    }
    // sync the histogram
    this->histred.AllReduce(BeginPtr(this->wspace.hset[0].data), this->wspace.hset[0].data.size());    
  }
  virtual void ResetPositionAfterSplit(IFMatrix *p_fmat,
                                       const RegTree &tree) {
    this->ResetPositionCol(this->qexpand, p_fmat, tree);
  }
  virtual void ResetPosAndPropose(const std::vector<bst_gpair> &gpair,
                                  IFMatrix *p_fmat,
                                  const BoosterInfo &info,
                                  const RegTree &tree) {
    this->GetNodeStats(gpair, *p_fmat, tree, info,
                       &thread_stats, &node_stats);
    sketchs.resize(this->qexpand.size() * tree.param.num_feature);
    for (size_t i = 0; i < sketchs.size(); ++i) {
      sketchs[i].Init(info.num_row, this->param.sketch_eps);
    }
    thread_sketch.resize(this->get_nthread());
    // number of rows in
    const size_t nrows = p_fmat->buffered_rowset().size();
    // start accumulating statistics
    utils::IIterator<ColBatch> *iter = p_fmat->ColIterator();
    iter->BeforeFirst();
    while (iter->Next()) {
      const ColBatch &batch = iter->Value();
      // start enumeration
      const bst_omp_uint nsize = static_cast<bst_omp_uint>(batch.size);
      #pragma omp parallel for schedule(dynamic, 1)
      for (bst_omp_uint i = 0; i < nsize; ++i) {
        this->UpdateSketchCol(gpair, batch[i], tree,
                              node_stats,
                              batch.col_index[i],
                              batch[i].length == nrows,                              
                              &thread_sketch[omp_get_thread_num()]);       
      }
    }
    // setup maximum size
    unsigned max_size = this->param.max_sketch_size();
    // synchronize sketch
    summary_array.resize(sketchs.size());
    for (size_t i = 0; i < sketchs.size(); ++i) {
      utils::WXQuantileSketch<bst_float, bst_float>::SummaryContainer out;
      sketchs[i].GetSummary(&out);
      summary_array[i].Reserve(max_size);
      summary_array[i].SetPrune(out, max_size);
    }
    size_t n4bytes = (WXQSketch::SummaryContainer::CalcMemCost(max_size) + 3) / 4;
    sreducer.AllReduce(BeginPtr(summary_array), n4bytes, summary_array.size());
    // now we get the final result of sketch, setup the cut
    this->wspace.cut.clear();
    this->wspace.rptr.clear();
    this->wspace.rptr.push_back(0);
    for (size_t wid = 0; wid < this->qexpand.size(); ++wid) {
      for (int fid = 0; fid < tree.param.num_feature; ++fid) {
        const WXQSketch::Summary &a = summary_array[wid * tree.param.num_feature + fid];
        for (size_t i = 1; i < a.size; ++i) {
          bst_float cpt = a.data[i].value - rt_eps;
          if (i == 1 || cpt > this->wspace.cut.back()) {
            this->wspace.cut.push_back(cpt);
          }
        }
        // push a value that is greater than anything
        if (a.size != 0) {
          bst_float cpt = a.data[a.size - 1].value;
          // this must be bigger than last value in a scale
          bst_float last = cpt + fabs(cpt) + rt_eps;
          this->wspace.cut.push_back(last);
        }
        this->wspace.rptr.push_back(this->wspace.cut.size());
      }
      // reserve last value for global statistics
      this->wspace.cut.push_back(0.0f);
      this->wspace.rptr.push_back(this->wspace.cut.size());
    }
    utils::Assert(this->wspace.rptr.size() ==
                  (tree.param.num_feature + 1) * this->qexpand.size() + 1,
                  "cut space inconsistent");
  }
  
 private:
  inline void UpdateHistCol(const std::vector<bst_gpair> &gpair,
                            const ColBatch::Inst &c,
                            const BoosterInfo &info,
                            const RegTree &tree,
                            bst_uint fid,
                            std::vector<HistEntry> *p_temp) {
    if (c.length == 0) return;
    // initialize sbuilder for use
    std::vector<HistEntry> &hbuilder = *p_temp;
    hbuilder.resize(tree.param.num_nodes);
    for (size_t i = 0; i < this->qexpand.size(); ++i) {
      const unsigned nid = this->qexpand[i];
      const unsigned wid = this->node2workindex[nid];
      hbuilder[nid].istart = 0;
      hbuilder[nid].hist = this->wspace.hset[0][fid + wid * (tree.param.num_feature+1)];
    }
    for (bst_uint j = 0; j < c.length; ++j) {
      const bst_uint ridx = c[j].index;
      const int nid = this->position[ridx];
      if (nid >= 0) {
        hbuilder[nid].Add(c[j].fvalue, gpair, info, ridx);
      }
    }
  }
  inline void UpdateSketchCol(const std::vector<bst_gpair> &gpair,
                              const ColBatch::Inst &c,
                              const RegTree &tree,
                              const std::vector<TStats> &nstats,
                              bst_uint fid,
                              bool col_full,
                              std::vector<BaseMaker::SketchEntry> *p_temp) {
    if (c.length == 0) return;
    // initialize sbuilder for use
    std::vector<BaseMaker::SketchEntry> &sbuilder = *p_temp;
    sbuilder.resize(tree.param.num_nodes);
    for (size_t i = 0; i < this->qexpand.size(); ++i) {
      const unsigned nid = this->qexpand[i];
      const unsigned wid = this->node2workindex[nid];
      sbuilder[nid].sum_total = 0.0f;
      sbuilder[nid].sketch = &sketchs[wid * tree.param.num_feature + fid];
    }

    if (!col_full) {
      // first pass, get sum of weight, TODO, optimization to skip first pass
      for (bst_uint j = 0; j < c.length; ++j) {
        const bst_uint ridx = c[j].index;
        const int nid = this->position[ridx];
        if (nid >= 0) {
          sbuilder[nid].sum_total += gpair[ridx].hess;
        }
      }
    } else {
      for (size_t i = 0; i < this->qexpand.size(); ++i) {
        const unsigned nid = this->qexpand[i];        
        sbuilder[nid].sum_total = nstats[nid].sum_hess;
      } 
    }
    // if only one value, no need to do second pass
    if (c[0].fvalue  == c[c.length-1].fvalue) {
      for (size_t i = 0; i < this->qexpand.size(); ++i) {
        const int nid = this->qexpand[i];
        sbuilder[nid].sketch->Push(c[0].fvalue, sbuilder[nid].sum_total);
      }
      return;
    }
    // two pass scan
    unsigned max_size = this->param.max_sketch_size();
    for (size_t i = 0; i < this->qexpand.size(); ++i) {
      const int nid = this->qexpand[i];
      sbuilder[nid].Init(max_size);
    }
    // second pass, build the sketch
    for (bst_uint j = 0; j < c.length; ++j) {
      const bst_uint ridx = c[j].index;
      const int nid = this->position[ridx];
      if (nid >= 0) {
        sbuilder[nid].Push(c[j].fvalue, gpair[ridx].hess, max_size);
      }
    }
    for (size_t i = 0; i < this->qexpand.size(); ++i) {
      const int nid = this->qexpand[i];
      sbuilder[nid].Finalize(max_size);
    }
  }
  
  // thread temp data
  std::vector< std::vector<BaseMaker::SketchEntry> > thread_sketch;
  // used to hold statistics
  std::vector< std::vector<TStats> > thread_stats;
  // used to hold start pointer
  std::vector< std::vector<HistEntry> > thread_hist;
  // node statistics
  std::vector<TStats> node_stats;
  // summary array
  std::vector< WXQSketch::SummaryContainer> summary_array;
  // reducer for summary
  sync::SerializeReducer<WXQSketch::SummaryContainer> sreducer;
  // per node, per feature sketch
  std::vector< utils::WXQuantileSketch<bst_float, bst_float> > sketchs;  
};

template<typename TStats>
class QuantileHistMaker: public HistMaker<TStats> {  
 protected:
  virtual void ResetPosAndPropose(const std::vector<bst_gpair> &gpair,
                                  IFMatrix *p_fmat,
                                  const BoosterInfo &info,
                                  const RegTree &tree) {
    // initialize the data structure
    int nthread = BaseMaker::get_nthread();
    sketchs.resize(this->qexpand.size() * tree.param.num_feature);
    for (size_t i = 0; i < sketchs.size(); ++i) {
      sketchs[i].Init(info.num_row, this->param.sketch_eps);
    }
    // start accumulating statistics
    utils::IIterator<RowBatch> *iter = p_fmat->RowIterator();
    iter->BeforeFirst();
    while (iter->Next()) {
      const RowBatch &batch = iter->Value();
      // parallel convert to column major format
      utils::ParallelGroupBuilder<SparseBatch::Entry> builder(&col_ptr, &col_data, &thread_col_ptr);
      builder.InitBudget(tree.param.num_feature, nthread);

      const bst_omp_uint nbatch = static_cast<bst_omp_uint>(batch.size);      
      #pragma omp parallel for schedule(static)
      for (bst_omp_uint i = 0; i < nbatch; ++i) {
        RowBatch::Inst inst = batch[i];
        const bst_uint ridx = static_cast<bst_uint>(batch.base_rowid + i);
        int nid = this->position[ridx];
        if (nid >= 0) {
          if (!tree[nid].is_leaf()) {
            this->position[ridx] = nid = HistMaker<TStats>::NextLevel(inst, tree, nid);
          } 
          if (this->node2workindex[nid] < 0) {
            this->position[ridx] = ~nid;
          } else{
            for (bst_uint j = 0; j < inst.length; ++j) { 
              builder.AddBudget(inst[j].index, omp_get_thread_num());
            }
          }
        }
      }
      builder.InitStorage();
      #pragma omp parallel for schedule(static)
      for (bst_omp_uint i = 0; i < nbatch; ++i) {
        RowBatch::Inst inst = batch[i];
        const bst_uint ridx = static_cast<bst_uint>(batch.base_rowid + i);
        const int nid = this->position[ridx];
        if (nid >= 0) {
          for (bst_uint j = 0; j < inst.length; ++j) {
            builder.Push(inst[j].index,
                         SparseBatch::Entry(nid, inst[j].fvalue),
                         omp_get_thread_num());
          }
        }
      }
      // start putting things into sketch
      const bst_omp_uint nfeat = col_ptr.size() - 1;
      #pragma omp parallel for schedule(dynamic, 1)
      for (bst_omp_uint k = 0; k < nfeat; ++k) {
        for (size_t i = col_ptr[k]; i < col_ptr[k+1]; ++i) {
          const SparseBatch::Entry &e = col_data[i];
          const int wid = this->node2workindex[e.index];
          sketchs[wid * tree.param.num_feature + k].Push(e.fvalue, gpair[e.index].hess);
        }
      }
    }
    // setup maximum size
    unsigned max_size = this->param.max_sketch_size();
    // synchronize sketch
    summary_array.resize(sketchs.size());
    for (size_t i = 0; i < sketchs.size(); ++i) {
      utils::WQuantileSketch<bst_float, bst_float>::SummaryContainer out;
      sketchs[i].GetSummary(&out);
      summary_array[i].Reserve(max_size);
      summary_array[i].SetPrune(out, max_size);
    }
    size_t n4bytes = (WXQSketch::SummaryContainer::CalcMemCost(max_size) + 3) / 4;
    sreducer.AllReduce(BeginPtr(summary_array), n4bytes, summary_array.size());
    // now we get the final result of sketch, setup the cut
    this->wspace.cut.clear();
    this->wspace.rptr.clear();
    this->wspace.rptr.push_back(0);
    for (size_t wid = 0; wid < this->qexpand.size(); ++wid) {
      for (int fid = 0; fid < tree.param.num_feature; ++fid) {
        const WXQSketch::Summary &a = summary_array[wid * tree.param.num_feature + fid];
        for (size_t i = 1; i < a.size; ++i) {
          bst_float cpt = a.data[i].value - rt_eps;
          if (i == 1 || cpt > this->wspace.cut.back()) {
            this->wspace.cut.push_back(cpt);
          }
        }
        // push a value that is greater than anything
        if (a.size != 0) {
          bst_float cpt = a.data[a.size - 1].value;
          // this must be bigger than last value in a scale
          bst_float last = cpt + fabs(cpt) + rt_eps;
          this->wspace.cut.push_back(last);
        }
        this->wspace.rptr.push_back(this->wspace.cut.size());
      }
      // reserve last value for global statistics
      this->wspace.cut.push_back(0.0f);
      this->wspace.rptr.push_back(this->wspace.cut.size());
    }
    utils::Assert(this->wspace.rptr.size() ==
                  (tree.param.num_feature + 1) * this->qexpand.size() + 1,
                  "cut space inconsistent");
  }

 private:
  typedef utils::WXQuantileSketch<bst_float, bst_float> WXQSketch;
  // summary array
  std::vector< WXQSketch::SummaryContainer> summary_array;
  // reducer for summary
  sync::SerializeReducer<WXQSketch::SummaryContainer> sreducer;
  // local temp column data structure
  std::vector<size_t> col_ptr;
  // local storage of column data
  std::vector<SparseBatch::Entry> col_data;
  std::vector< std::vector<size_t> > thread_col_ptr;
  // per node, per feature sketch
  std::vector< utils::WQuantileSketch<bst_float, bst_float> > sketchs;
};

}  // namespace tree
}  // namespace xgboost
#endif  // XGBOOST_TREE_UPDATER_HISTMAKER_INL_HPP_
