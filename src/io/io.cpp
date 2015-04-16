#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_DEPRECATE
#define NOMINMAX
#include <string>
#include "./io.h"
#include "../utils/io.h"
#include "../utils/utils.h"
#include "simple_dmatrix-inl.hpp"
#include "page_dmatrix-inl.hpp"

namespace xgboost {
namespace io {
DataMatrix* LoadDataMatrix(const char *fname,
                           bool silent,
                           bool savebuffer,
                           bool loadsplit,
                           const char *cache_file) {
  std::string fname_ = fname;
  const char *dlm = strchr(fname, '#');
  if (dlm != NULL) {
    utils::Check(strchr(dlm + 1, '#') == NULL,
                 "only one `#` is allowed in file path for cachefile specification");
    utils::Check(cache_file == NULL,
                 "can only specify the cachefile with `#` or argument, not both");
    fname_ = std::string(fname, dlm - fname);
    fname = fname_.c_str();
    cache_file = dlm +1;
  }
  
  if (cache_file == NULL) { 
    if (!std::strcmp(fname, "stdin") ||
        !std::strncmp(fname, "s3://", 5) ||
        !std::strncmp(fname, "hdfs://", 7) ||
        loadsplit) {
      DMatrixSimple *dmat = new DMatrixSimple();
      dmat->LoadText(fname, silent, loadsplit);
      return dmat;
    }
    int magic;
    utils::FileStream fs(utils::FopenCheck(fname, "rb"));
    utils::Check(fs.Read(&magic, sizeof(magic)) != 0, "invalid input file format");
    fs.Seek(0);
    if (magic == DMatrixSimple::kMagic) { 
      DMatrixSimple *dmat = new DMatrixSimple();
      dmat->LoadBinary(fs, silent, fname);
      fs.Close();
      return dmat;
    }
    fs.Close();
    DMatrixSimple *dmat = new DMatrixSimple();
    dmat->CacheLoad(fname, silent, savebuffer);
    return dmat;
  } else {
    FILE *fi = fopen64(cache_file, "rb");
    if (fi != NULL) {
      DMatrixPage *dmat = new DMatrixPage();
      utils::FileStream fs(fi);
      dmat->LoadBinary(fs, silent, cache_file);
      fs.Close();
      return dmat;
    } else {   
      DMatrixPage *dmat = new DMatrixPage();
      dmat->LoadText(fname, cache_file, false, loadsplit);
      return dmat;
    }
  }
}

void SaveDataMatrix(const DataMatrix &dmat, const char *fname, bool silent) {  
  if (dmat.magic == DMatrixSimple::kMagic) {
    const DMatrixSimple *p_dmat = static_cast<const DMatrixSimple*>(&dmat);
    p_dmat->SaveBinary(fname, silent);
  } else {
    DMatrixSimple smat;
    smat.CopyFrom(dmat);
    smat.SaveBinary(fname, silent);
  }
}

}  // namespace io
}  // namespace xgboost
