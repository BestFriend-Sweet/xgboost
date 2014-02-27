#ifndef _XGBOOST_TREE_HPP_
#define _XGBOOST_TREE_HPP_
/*!
 * \file xgboost_tree.hpp
 * \brief implementation of regression tree
 * \author Tianqi Chen: tianqi.tchen@gmail.com 
 */
#include "xgboost_tree_model.h"

namespace xgboost{
    namespace booster{
        const bool rt_debug = false;
        // whether to check bugs
        const bool check_bug = false;
    
        const float rt_eps = 1e-5f;
        const float rt_2eps = rt_eps * 2.0f;
        
        inline double sqr( double a ){
            return a * a;
        }
    };
};

#include "xgboost_svdf_tree.hpp"

namespace xgboost{
    namespace booster{
        // regression tree, construction algorithm is seperated from this class
        // see RegTreeUpdater
        class RegTreeTrainer : public IBooster{
        public:
            RegTreeTrainer( void ){ silent = 0; }
            virtual ~RegTreeTrainer( void ){}
        public:
            virtual void SetParam( const char *name, const char *val ){
                if( !strcmp( name, "silent") )  silent = atoi( val );
                param.SetParam( name, val );
                tree.param.SetParam( name, val );
            }
            virtual void LoadModel( utils::IStream &fi ){
                tree.LoadModel( fi );
            }
            virtual void SaveModel( utils::IStream &fo ) const{
                tree.SaveModel( fo );
            }
            virtual void InitModel( void ){
                tree.InitModel();
            }
        public:
            virtual void DoBoost( std::vector<float> &grad, 
                                  std::vector<float> &hess,
                                  const FMatrixS &smat,
                                  const std::vector<unsigned> &group_id ){
                this->DoBoost_( grad, hess, smat, group_id );
            }
            
            virtual int GetLeafIndex( const std::vector<float> &feat,
                                      const std::vector<bool>  &funknown,
                                      unsigned gid = 0 ){
                // start from groups that belongs to current data
                int pid = (int)gid;
                // tranverse tree
                while( !tree[ pid ].is_leaf() ){
                    unsigned split_index = tree[ pid ].split_index();
                    pid = this->GetNext( pid, feat[ split_index ], funknown[ split_index ] );
                }
                return pid;
            }

            virtual void PredPath( std::vector<int> &path, const FMatrixS::Line &feat, unsigned gid = 0 ){
                path.clear();
                this->InitTmp();
                this->PrepareTmp( feat );

                int pid = (int)gid;
                path.push_back( pid );
                // tranverse tree
                while( !tree[ pid ].is_leaf() ){                    
                    unsigned split_index = tree[ pid ].split_index();
                    pid = this->GetNext( pid, tmp_feat[ split_index ], tmp_funknown[ split_index ] );
                    path.push_back( pid );
                }                
                this->DropTmp( feat );
            }

            virtual float Predict( const FMatrixS::Line &feat, unsigned gid = 0 ){
                this->InitTmp();
                this->PrepareTmp( feat );
                int pid = this->GetLeafIndex( tmp_feat, tmp_funknown, gid );
                this->DropTmp( feat );
                return tree[ pid ].leaf_value();
            }
            virtual float Predict( const std::vector<float> &feat, 
                                   const std::vector<bool>  &funknown,
                                   unsigned gid = 0 ){
                utils::Assert( feat.size() >= (size_t)tree.param.num_feature,
                               "input data smaller than num feature" );
                int pid = this->GetLeafIndex( feat, funknown, gid );
                return tree[ pid ].leaf_value();
            }
            
            virtual void DumpModel( FILE *fo ){
                tree.DumpModel( fo );
            }
        private:
            template<typename FMatrix>
            inline void DoBoost_( std::vector<float> &grad, 
                                  std::vector<float> &hess,
                                  const FMatrix &smat,
                                  const std::vector<unsigned> &group_id ){
                utils::Assert( grad.size() < UINT_MAX, "number of instance exceed what we can handle" );
                if( !silent ){
                    printf( "\nbuild GBRT with %u instances\n", (unsigned)grad.size() );
                }
                // start with a id set
                RTreeUpdater<FMatrix> updater( param, tree, grad, hess, smat, group_id );
                int num_pruned;
                tree.param.max_depth = updater.do_boost( num_pruned );
                
                if( !silent ){
                    printf( "tree train end, %d roots, %d extra nodes, %d pruned nodes ,max_depth=%d\n", 
                            tree.param.num_roots, tree.num_extra_nodes(), num_pruned, tree.param.max_depth );
                }
            }
        private:
            int silent;
            RegTree tree;
            TreeParamTrain param;
        private:
            std::vector<float> tmp_feat;
            std::vector<bool>  tmp_funknown;
            inline void InitTmp( void ){
                if( tmp_feat.size() != (size_t)tree.param.num_feature ){
                    tmp_feat.resize( tree.param.num_feature );
                    tmp_funknown.resize( tree.param.num_feature );
                    std::fill( tmp_funknown.begin(), tmp_funknown.end(), true );
                }
            }
            inline void PrepareTmp( const FMatrixS::Line &feat ){
                for( unsigned i = 0; i < feat.len; i ++ ){
                    utils::Assert( feat[i].findex < (unsigned)tmp_funknown.size() , "input feature execeed bound" );
                    tmp_funknown[ feat[i].findex ] = false;
                    tmp_feat[ feat[i].findex ] = feat[i].fvalue;
                } 
            }
            inline void DropTmp( const FMatrixS::Line &feat ){
                for( unsigned i = 0; i < feat.len; i ++ ){
                    tmp_funknown[ feat[i].findex ] = true;
                }
            }

            inline int GetNext( int pid, float fvalue, bool is_unknown ){
                float split_value = tree[ pid ].split_cond();
                if( is_unknown ){
                    if( tree[ pid ].default_left() ) return tree[ pid ].cleft();
                else return tree[ pid ].cright();
                }else{
                    if( fvalue < split_value ) return tree[ pid ].cleft();
                    else return tree[ pid ].cright();
                }
            }
        };
    };
};

#endif

