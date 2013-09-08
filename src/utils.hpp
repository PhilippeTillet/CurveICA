#ifndef PARICA_UTILS_HPP_
#define PARICA_UTILS_HPP_

#include "blas_backend.hpp"

namespace parica{

    namespace detail{

        template<class ScalarType>
        inline ScalarType get_ij(ScalarType* A, std::size_t /*M*/, std::size_t N, std::size_t i, std::size_t j){
            return A[i*N+j];
        }

        template<int N>
        struct compile_time_pow{
            template<class ScalarType>
            ScalarType operator()(ScalarType v){
                return v*compile_time_pow<N-1>()(v);
            }
        };

        template<>
        struct compile_time_pow<0>{
            template<class ScalarType>
            ScalarType operator()(ScalarType v){
                return 1;
            }
        };

        template<class ScalarType>
        static void inplace_inverse(lapack_int order, std::size_t N, ScalarType * A)
        {
            int *ipiv = new int[N+1];
            int info;
            info = blas_backend<ScalarType>::getrf(order,N,N,A,N,ipiv);
            info = blas_backend<ScalarType>::getri(order,N,A,N,ipiv);
            delete ipiv;
        }

    }

}

#endif
