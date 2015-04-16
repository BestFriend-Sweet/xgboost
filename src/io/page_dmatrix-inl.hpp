#ifndef XGBOOST_IO_PAGE_DMATRIX_INL_HPP_
#define XGBOOST_IO_PAGE_DMATRIX_INL_HPP_
/*!
 * \file page_dmatrix-inl.hpp
 *   row iterator based on sparse page
 * \author Tianqi Chen
 */
#include <vector>
#include "../data.h"
#include "../utils/iterator.h"
#include "../utils/thread_buffer.h"
#include "./simple_fmatrix-inl.hpp"
#include "./sparse_batch_page.h"
#include "./page_fmatrix-inl.hpp"

namespace xgboost {
namespace io {
/*! \brief thread buffer iterator */
class ThreadRowPageIterator: public utils::IIterator<RowBatch> {
 public:
  ThreadRowPageIterator(void) {
    itr.SetParam("buffer_size", "2");
    page_ = NULL;
    base_rowid_ = 0;
  }
  virtual ~ThreadRowPageIterator(void) {}
  virtual void Init(void) {
  }
  virtual void BeforeFirst(void) {
    itr.BeforeFirst();
    base_rowid_ = 0;
  }
  virtual bool Next(void) {
    if (!itr.Next(page_)) return false;
    out_.base_rowid  = base_rowid_;
    out_.ind_ptr = BeginPtr(page_->offset);
    out_.data_ptr = BeginPtr(page_->data);
    out_.size = page_->offset.size() - 1;
    base_rowid_ += out_.size;
    return true;
  }
  virtual const RowBatch &Value(void) const {
    return out_;
  }
  /*! \brief load and initialize the iterator with fi */
  inline void Load(const utils::FileStream &fi) {
    itr.get_factory().SetFile(fi, 0);
    itr.Init();
    this->BeforeFirst();
  }

 private:
  // base row id
  size_t base_rowid_;
  // output data
  RowBatch out_;
  SparsePage *page_;
  utils::ThreadBuffer<SparsePage*, SparsePageFactory> itr;
};

/*! \brief data matrix using page */
template<int TKMagic>
class DMatrixPageBase : public DataMatrix {
 public:
  DMatrixPageBase(void) : DataMatrix(kMagic) {
    iter_ = new ThreadRowPageIterator();
  }
  // virtual destructor
  virtual ~DMatrixPageBase(void) {
    // do not delete row iterator, since it is owned by fmat
    // to be cleaned up in a more clear way
  }
  /*! \brief load and initialize the iterator with fi */
  inline void LoadBinary(utils::FileStream &fi,
                         bool silent,
                         const char *fname_) {
    std::string fname = fname_;
    int tmagic;
    utils::Check(fi.Read(&tmagic, sizeof(tmagic)) != 0, "invalid input file format");
    utils::Check(tmagic == magic, "invalid format,magic number mismatch");
    this->info.LoadBinary(fi);
    // load in the row data file
    fname += ".row.blob";
    utils::FileStream fs(utils::FopenCheck(fname.c_str(), "rb"));
    iter_->Load(fs);
    if (!silent) {
      utils::Printf("DMatrixPage: %lux%lu matrix is loaded",
                    static_cast<unsigned long>(info.num_row()),
                    static_cast<unsigned long>(info.num_col()));
      if (fname_ != NULL) {
        utils::Printf(" from %s\n", fname_);
      } else {
        utils::Printf("\n");
      }
      if (info.group_ptr.size() != 0) {
        utils::Printf("data contains %u groups\n", (unsigned)info.group_ptr.size() - 1);
      }
    }
  }
  /*! \brief save a DataMatrix as DMatrixPage */
  inline static void Save(const char *fname_, const DataMatrix &mat, bool silent) {
    std::string fname = fname_;
    utils::FileStream fs(utils::FopenCheck(fname.c_str(), "wb"));
    int magic = kMagic;
    fs.Write(&magic, sizeof(magic));
    mat.info.SaveBinary(fs);
    fs.Close();
    fname += ".row.blob";
    utils::IIterator<RowBatch> *iter = mat.fmat()->RowIterator();
    utils::FileStream fbin(utils::FopenCheck(fname.c_str(), "wb"));
    SparsePage page;
    iter->BeforeFirst();
    while (iter->Next()) {
      const RowBatch &batch = iter->Value();
      for (size_t i = 0; i < batch.size; ++i) {
        page.Push(batch[i]);
        if (page.MemCostBytes() >= kPageSize) {
          page.Save(&fbin); page.Clear();
        }
      }
    }
    if (page.data.size() != 0) page.Save(&fbin);
    fbin.Close();
    if (!silent) {
      utils::Printf("DMatrixPage: %lux%lu is saved to %s\n",
                    static_cast<unsigned long>(mat.info.num_row()),
                    static_cast<unsigned long>(mat.info.num_col()), fname_);
    }
  }
  /*! \brief save a LibSVM format file as DMatrixPage */
  inline void LoadText(const char *uri,
                       const char* cache_file,
                       bool silent,
                       bool loadsplit) {
    int rank = 0, npart = 1;
    if (loadsplit) {
      rank = rabit::GetRank();
      npart = rabit::GetWorldSize();
    }
    std::string fname_row = std::string(cache_file) + ".row.blob";
    utils::FileStream fo(utils::FopenCheck(fname_row.c_str(), "wb"));
    SparsePage page;
    dmlc::InputSplit *in =
        dmlc::InputSplit::Create(uri, rank, npart);
    std::string line;
    info.Clear();
    while (in->ReadRecord(&line)) {
      float label;
      std::istringstream ss(line);
      std::vector<RowBatch::Entry> feats;
      ss >> label;
      while (!ss.eof()) {
        RowBatch::Entry e;
        if (!(ss >> e.index)) break;
        ss.ignore(32, ':');
        if (!(ss >> e.fvalue)) break;
        feats.push_back(e);
      }
      RowBatch::Inst row(BeginPtr(feats), feats.size());
      page.Push(row);
      if (page.MemCostBytes() >= kPageSize) {
        page.Save(&fo); page.Clear();
      }
      for (size_t i = 0; i < feats.size(); ++i) {
        info.info.num_col = std::max(info.info.num_col,
                                     static_cast<size_t>(feats[i].index+1));
      }
      this->info.labels.push_back(label);
      info.info.num_row += 1;
    }
    if (page.data.size() != 0) {
      page.Save(&fo);
    }
    delete in;
    fo.Close();    
    iter_->Load(utils::FileStream(utils::FopenCheck(fname_row.c_str(), "rb")));    
    // save data matrix
    utils::FileStream fs(utils::FopenCheck(cache_file, "wb"));
    int magic = kMagic;
    fs.Write(&magic, sizeof(magic));
    this->info.SaveBinary(fs);
    fs.Close();
    if (!silent) {
      utils::Printf("DMatrixPage: %lux%lu is parsed from %s\n",
                    static_cast<unsigned long>(info.num_row()),
                    static_cast<unsigned long>(info.num_col()),
                    uri);
    }
  }
  /*! \brief magic number used to identify DMatrix */
  static const int kMagic = TKMagic;
  /*! \brief page size 64 MB */
  static const size_t kPageSize = 64 << 18;
 protected:
  /*! \brief row iterator */
  ThreadRowPageIterator *iter_;
};

class DMatrixPage : public DMatrixPageBase<0xffffab02> {
 public:
  DMatrixPage(void) {
    fmat_ = new FMatrixS(iter_);
  }
  virtual ~DMatrixPage(void) {
    delete fmat_;
  }
  virtual IFMatrix *fmat(void) const {
    return fmat_;
  }
  /*! \brief the real fmatrix */
  IFMatrix *fmat_;
};
}  // namespace io
}  // namespace xgboost
#endif  // XGBOOST_IO_PAGE_ROW_ITER_INL_HPP_
