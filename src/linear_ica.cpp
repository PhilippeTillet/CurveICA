/* ===========================
 *
 * Copyright (c) 2013 Philippe Tillet - National Chiao Tung University
 *
 * CLICA - Hybrid ICA using ViennaCL + Eigen
 *
 * License : MIT X11 - See the LICENSE file in the root folder
 * ===========================*/

#include "Eigen/Dense"

#include "tests/benchmark-utils.hpp"

#include "fmincl/minimize.hpp"
#include "fmincl/backends/openblas.hpp"

#include "src/whiten.hpp"
#include "src/utils.hpp"


namespace parica{

template<class ScalarType>
struct ica_functor{
private:
    static const int alpha_sub = 4;
    static const int alpha_super = 1;
private:
    template <typename T>
    inline int sgn(T val) const {
        return (val>0)?1:-1;
    }
public:
    ica_functor(ScalarType const * data, std::size_t nchans, std::size_t nframes) : data_(data), nchans_(nchans), nframes_(nframes){
        ipiv_ =  new int[nchans_+1];

        z1 = new ScalarType[nchans_*nframes_];
        phi = new ScalarType[nchans_*nframes_];
        phi_z1t = new ScalarType[nchans_*nchans_];
        dweights = new ScalarType[nchans_*nchans_];
        dbias = new ScalarType[nchans_];
        W = new ScalarType[nchans_*nchans_];
        WLU = new ScalarType[nchans_*nchans_];
        b_ = new ScalarType[nchans_];
        alpha = new ScalarType[nchans_];
        means_logp = new ScalarType[nchans_];
    }

    ~ica_functor(){
        delete ipiv_;
    }

    ScalarType operator()(ScalarType const * x, ScalarType ** grad) const {

        Timer t;
        t.start();

        //Rerolls the variables into the appropriates datastructures
        std::memcpy(W, x,sizeof(ScalarType)*nchans_*nchans_);
        std::memcpy(b_, x+nchans_*nchans_, sizeof(ScalarType)*nchans_);



        //z1 = W*data_;
        blas_backend<ScalarType>::gemm(CblasRowMajor,CblasNoTrans,CblasNoTrans,nchans_,nframes_,nchans_,1,W,nchans_,data_,nframes_,0,z1,nframes_);


        //z2 = z1 + b(:, ones(nframes_,1));
        //kurt = (mean(z2.^2,2).^2) ./ mean(z2.^4,2) - 3
        //alpha = alpha_sub*(kurt<0) + alpha_super*(kurt>0)
        for(unsigned int i = 0 ; i < nchans_ ; ++i){
            ScalarType m2 = 0, m4 = 0;
            ScalarType b = b_[i];
            for(unsigned int j = 0; j < nframes_ ; j++){
                ScalarType val = z1[i*nframes_+j] + b;
                m2 += std::pow(val,2);
                m4 += std::pow(val,4);
            }
            m2 = std::pow(1/(ScalarType)nframes_*m2,2);
            m4 = 1/(ScalarType)nframes_*m4;
            ScalarType kurt = m4/m2 - 3;
            alpha[i] = alpha_sub*(kurt<0) + alpha_super*(kurt>=0);
        }


        //mata = alpha(:,ones(nframes_,1));
        //logp = log(mata) - log(2) - gammaln(1./mata) - abs(z2).^mata;
        for(unsigned int i = 0 ; i < nchans_ ; ++i){
            ScalarType current = 0;
            ScalarType a = alpha[i];
            ScalarType b = b_[i];
            for(unsigned int j = 0; j < nframes_ ; j++){
                ScalarType val = z1[i*nframes_+j] + b;
                ScalarType fabs_val = std::fabs(val);
                current += (a==alpha_sub)?detail::compile_time_pow<alpha_sub>()(fabs_val):detail::compile_time_pow<alpha_super>()(fabs_val);
            }
            means_logp[i] = -1/(ScalarType)nframes_*current + std::log(a) - std::log(2) - lgamma(1/a);
        }

        //H = log(abs(det(w))) + sum(means_logp);

        //LU Decomposition
        std::memcpy(WLU,W,sizeof(ScalarType)*nchans_*nchans_);
        blas_backend<ScalarType>::getrf(LAPACK_ROW_MAJOR,nchans_,nchans_,WLU,nchans_,ipiv_);

        //det = prod(diag(WLU))
        ScalarType absdet = 1;
        for(std::size_t i = 0 ; i < nchans_ ; ++i)
            absdet*=std::abs(WLU[i*nchans_+i]);

        ScalarType H = std::log(absdet);
        for(std::size_t i = 0; i < nchans_ ; ++i)
            H+=means_logp[i];

        if(grad){

            //phi = mean(mata.*abs(z2).^(mata-1).*sign(z2),2);
            for(unsigned int i = 0 ; i < nchans_ ; ++i){
                ScalarType a = alpha[i];
                ScalarType b = b_[i];
                for(unsigned int j = 0 ; j < nframes_ ; ++j){
                    ScalarType val = z1[i*nframes_+j] + b;
                    ScalarType fabs_val = std::fabs(val);
                    ScalarType fabs_val_pow = (a==alpha_sub)?detail::compile_time_pow<alpha_sub-1>()(fabs_val):detail::compile_time_pow<alpha_super-1>()(fabs_val);
                    phi[i*nframes_+j] = a*fabs_val_pow*sgn(val);
                }
            }

            //dbias = mean(phi,2)
            detail::mean(phi,nchans_,nframes_,dbias);

            /*dweights = -(eye(N) - 1/n*phi*z1')*inv(W)'*/

            //WLU = inv(W)
            blas_backend<ScalarType>::getri(LAPACK_ROW_MAJOR,nchans_,WLU,nchans_,ipiv_);

            //lhs = I(N,N) - 1/N*phi*z1')
            blas_backend<ScalarType>::gemm(CblasRowMajor,CblasNoTrans,CblasTrans,nchans_,nchans_,nframes_ ,-1/(ScalarType)nframes_,phi,nframes_,z1,nframes_,0,phi_z1t,nchans_);
            for(std::size_t i = 0 ; i < nchans_ ; ++i)
                phi_z1t[i*nchans_+i] += 1;

            //dweights = -lhs*Winv'
            blas_backend<ScalarType>::gemm(CblasRowMajor, CblasNoTrans,CblasTrans,nchans_,nchans_,nchans_,-1,phi_z1t,nchans_,WLU,nchans_,0,dweights,nchans_);

            //Copy back
            std::memcpy(*grad, dweights,sizeof(ScalarType)*nchans_*nchans_);
            std::memcpy(*grad+nchans_*nchans_, dbias, sizeof(ScalarType)*nchans_);
        }

        return -H;
    }

private:
    ScalarType const * data_;
    std::size_t nchans_;
    std::size_t nframes_;

    int *ipiv_;

    ScalarType* z1;
    ScalarType* phi;
    ScalarType* phi_z1t;
    ScalarType* dweights;
    ScalarType* dbias;
    ScalarType* W;
    ScalarType* WLU;
    ScalarType* b_;
    ScalarType* alpha;
    ScalarType* means_logp;
};


fmincl::optimization_options make_default_options(){
    fmincl::optimization_options options;
    options.direction = new fmincl::quasi_newton_tag();
    options.max_iter = 100;
    options.verbosity_level = 0;
    return options;
}


template<class DataType, class OutType>
void inplace_linear_ica(DataType const & data, OutType & out, fmincl::optimization_options const & options){
    typedef typename DataType::Scalar ScalarType;
    typedef typename result_of::internal_matrix_type<ScalarType>::type MatrixType;
    typedef typename result_of::internal_vector_type<ScalarType>::type VectorType;

    size_t nchans = data.rows();
    size_t nframes = data.cols();

    MatrixType data_copy(data);

    MatrixType W(nchans,nchans);
    VectorType b(nchans);

    std::size_t N = nchans*nchans + nchans;

    //Optimization Vector

    //Solution vector
    VectorType S(N);
    //Initial guess
    VectorType X = VectorType::Zero(N);
    for(unsigned int i = 0 ; i < nchans; ++i) X[i*(nchans+1)] = 1;
    for(unsigned int i = nchans*nchans ; i < nchans*(nchans+1) ; ++i) X[i] = 0;

    //Whiten Data
    MatrixType white_data(nchans, nframes);
    whiten<ScalarType>(nchans, nframes, data_copy.data(),white_data.data());

    ica_functor<ScalarType> fun(white_data.data(),nchans,nframes);
//    fmincl::utils::check_grad(fun,X);
    ScalarType* Sptr = S.data();
    ScalarType* Xptr = X.data();
    fmincl::minimize<fmincl::backend::OpenBlasTypes<ScalarType> >(Sptr,fun,Xptr,N,options);


    //Copies into datastructures
    std::memcpy(W.data(), S.data(),sizeof(ScalarType)*nchans*nchans);
    std::memcpy(b.data(), S.data()+nchans*nchans, sizeof(ScalarType)*nchans);

    //out = W*white_data;
    blas_backend<ScalarType>::gemm(CblasRowMajor,CblasNoTrans,CblasNoTrans,nchans,nframes,nchans,1,W.data(),nchans,white_data.data(),nframes,0,out.data(),nframes);
    out.colwise() += b;
}


typedef result_of::internal_matrix_type<double>::type MatD;
typedef result_of::internal_matrix_type<float>::type MatF;
template void inplace_linear_ica<MatD,MatD>(MatD  const & data, MatD & out, fmincl::optimization_options const & options);
template void inplace_linear_ica<MatF,MatF>(MatF  const & data, MatF & out, fmincl::optimization_options const & options);

}

