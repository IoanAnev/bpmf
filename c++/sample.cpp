/*
 * Copyright (c) 2014-2016, imec
 * All rights reserved.
 */


#include <random>
#include <memory>
#include <cstdio>
#include <iostream>
#include <climits>
#include <stdexcept>

#include "error.h"
#include "bpmf.h"
#include "io.h"

static const bool measure_perf = false;

std::ostream *Sys::os;
int Sys::procid = -1;
int Sys::nprocs = -1;

int Sys::nsims;
int Sys::burnin;
int Sys::update_freq;
double Sys::alpha = 2.0;

std::string Sys::odirname = "";

bool Sys::permute = true;
bool Sys::verbose = false;

// verifies that A has the same non-zero structure as B
void assert_same_struct(SparseMatrixD &A, SparseMatrixD &B)
{
    assert(A.cols() == B.cols());
    assert(A.rows() == B.rows());
    for(int i=0; i<A.cols(); ++i) assert(A.col(i).nonZeros() == B.col(i).nonZeros());
}

//
// Does predictions for prediction matrix T
// Computes RMSE (Root Means Square Error)
//
void Sys::predict(Sys& other, bool all)
{
    int n = (iter < burnin) ? 0 : (iter - burnin);
   
    double se(0.0); // squared err
    double se_avg(0.0); // squared avg err
    unsigned nump(0); // number of predictions

    int lo = all ? 0 : from();
    int hi = all ? num() : to();
    #pragma omp parallel for reduction(+:se,se_avg,nump)
    for(int k = lo; k<hi; k++) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(T,k); it; ++it)
        {
#ifdef BPMF_REDUCE
            if (it.row() >= other.to() || it.row() < other.from())
                continue;
#endif
            auto m = items().col(it.col());
            auto u = other.items().col(it.row());

            assert(m.norm() > 0.0);
            assert(u.norm() > 0.0);

            const double pred = m.dot(u) + mean_rating;
            se += sqr(it.value() - pred);

            // update average prediction
            double &avg = Pavg.coeffRef(it.row(), it.col());
            double delta = pred - avg;
            avg = (n == 0) ? pred : (avg + delta/n);
            double &m2 = Pm2.coeffRef(it.row(), it.col());
            m2 = (n == 0) ? 0 : m2 + delta * (pred - avg);
            se_avg += sqr(it.value() - avg);

            nump++;
        }
    }

    rmse = sqrt( se / nump );
    rmse_avg = sqrt( se_avg / nump );
}

//
// Prints sampling progress
//
void Sys::print(double items_per_sec, double ratings_per_sec, double norm_u, double norm_m) {
  char buf[1024];
  std::string phase = (iter < Sys::burnin) ? "Burnin" : "Sampling";
  sprintf(buf, "%d: %s iteration %d:\t RMSE: %3.4f\tavg RMSE: %3.4f\tFU(%6.2f)\tFM(%6.2f)\titems/sec: %6.2f\tratings/sec: %6.2fM\n",
                    Sys::procid, phase.c_str(), iter, rmse, rmse_avg, norm_u, norm_m, items_per_sec, ratings_per_sec / 1e6);
  Sys::cout() << buf;
}

//
// Constructor with that reads MTX files
// 
Sys::Sys(std::string name, std::string fname, std::string probename)
    : name(name), iter(-1), assigned(false), dom(nprocs+1)
{

    read_matrix(fname, M);
    read_matrix(probename, T);

    auto rows = std::max(M.rows(), T.rows());
    auto cols = std::max(M.cols(), T.cols());
    M.conservativeResize(rows,cols);
    T.conservativeResize(rows,cols);
    Pm2 = Pavg = Torig = T; // reference ratings and predicted ratings
    assert(M.rows() == Pavg.rows());
    assert(M.cols() == Pavg.cols());
    assert(Sys::nprocs <= (int)Sys::max_procs);
}

//
// Constructs Sys as transpose of existing Sys
//
Sys::Sys(std::string name, const SparseMatrixD &Mt, const SparseMatrixD &Pt) : name(name), iter(-1), assigned(false), dom(nprocs+1) {
    M = Mt.transpose();
    Pm2 = Pavg = T = Torig = Pt.transpose(); // reference ratings and predicted ratings
    assert(M.rows() == Pavg.rows());
    assert(M.cols() == Pavg.cols());
}

Sys::~Sys() 
{
    if (measure_perf) {
        Sys::cout() << " --------------------\n";
        Sys::cout() << name << ": sampling times on " << procid << "\n";
        for(int i = from(); i<to(); ++i) 
        {
            Sys::cout() << "\t" << nnz(i) << "\t" << sample_time.at(i) / nsims  << "\n";
        }
        Sys::cout() << " --------------------\n\n";
    }
}

//
// Intializes internal Matrices and Vectors
//
void Sys::init()
{
    //-- M
    assert(M.rows() > 0 && M.cols() > 0);
    mean_rating = M.sum() / M.nonZeros();
    items().setZero();
    sum_map().setZero();
    cov_map().setZero();
    norm_map().setZero();
    col_permutation.setIdentity(num());

#ifdef BPMF_REDUCE
    precMu = std::vector<VectorNd>(num(), VectorNd::Zero());
    precLambda = std::vector<MatrixNNd>(num(), MatrixNNd::Zero());
#endif

    Sys::cout() << "mean rating: " << mean_rating << std::endl;
    Sys::cout() << "total number of ratings in train: " << M.nonZeros() << std::endl;
    Sys::cout() << "total number of ratings in test: " << T.nonZeros() << std::endl;
    Sys::cout() << "average ratings per row: " << (double)M.nonZeros() / (double)M.cols() << std::endl;
    Sys::cout() << "num " << name << ": " << num() << std::endl;

    if (measure_perf) sample_time.resize(num(), .0);
}

class PrecomputedLLT : public Eigen::LLT<MatrixNNd>
{
  public:
    void operator=(const MatrixNNd &m) { m_matrix = m; m_isInitialized = true; m_info = Eigen::Success; }
};


//
// Update ONE movie or one user
//
VectorNd Sys::sample(long idx, Sys &other)
{
    auto start = tick();

    VectorNd rr = hp.LambdaF * hp.mu;                // vector num_latent x 1, we will use it in formula (14) from the paper
    PrecomputedLLT chol;                             // matrix num_latent x num_latent, chol="lambda_i with *" from formula (14)

    MatrixNNd MM(MatrixNNd::Zero());

#ifdef BPMF_REDUCE
    rr += precMu.at(idx);
    MM += precLambda.at(idx);
#else
    for (SparseMatrixD::InnerIterator it(M, idx); it; ++it)
    {
        auto col = other.items().col(it.row());
        MM.triangularView<Eigen::Upper>() += col * col.transpose();
        rr.noalias() += col * ((it.value() - mean_rating) * alpha);
    }
#endif
    
    // copy upper -> lower part, matrix is symmetric.
    MM.triangularView<Eigen::Lower>() = MM.transpose();

    chol.compute(hp.LambdaF + alpha * MM);

    if(chol.info() != Eigen::Success) THROWERROR("Cholesky failed");

    // now we should calculate formula (14) from the paper
    // u_i for k-th iteration = Gaussian distribution N(u_i | mu_i with *, [lambda_i with *]^-1) =
    //                        = mu_i with * + s * [U]^-1, 
    //                        where 
    //                              s is a random vector with N(0, I),
    //                              mu_i with * is a vector num_latent x 1, 
    //                              mu_i with * = [lambda_i with *]^-1 * rr,
    //                              lambda_i with * = L * U       

    // Expression u_i = U \ (s + (L \ rr)) in Matlab looks for Eigen library like: 

    chol.matrixL().solveInPlace(rr);                    // L*Y=rr => Y=L\rr, we store Y result again in rr vector  
    rr += nrandn();                                     // rr=s+(L\rr), we store result again in rr vector
    chol.matrixU().solveInPlace(rr);                    // u_i=U\rr 
    items().col(idx) = rr;                              // we save rr vector in items matrix (it is user features matrix)

#ifdef BPMF_REDUCE
    #pragma omp critical
    for (SparseMatrixD::InnerIterator it(M, idx); it; ++it)
    {
        other.precLambda.at(it.row()).triangularView<Eigen::Upper>() += rr * rr.transpose();
        other.precMu.at(it.row()).noalias() += rr * (it.value() - mean_rating) * alpha;
    }
#endif

    auto stop = tick();
    register_time(idx, 1e6 * (stop - start));
    //Sys::cout() << "  " << count << ": " << 1e6*(stop - start) << std::endl;

    assert(rr.norm() > .0);

    return rr;
}

// 
// update ALL movies / users in parallel
//
void Sys::sample(Sys &other) 
{
    iter++;
    thread_vector<VectorNd>  sums(VectorNd::Zero()); // sum
    thread_vector<double>    norms(0.0); // squared norm
    thread_vector<MatrixNNd> prods(MatrixNNd::Zero()); // outer prod

#ifdef BPMF_REDUCE
    other.precMu = std::vector<VectorNd>(other.num(), VectorNd::Zero());
    other.precLambda = std::vector<MatrixNNd>(other.num(), MatrixNNd::Zero());
#endif

#pragma omp parallel for schedule(guided)
    for (int i = from(); i < to(); ++i)
    {
#pragma omp task
        {
            auto r = sample(i, other);

            MatrixNNd cov = (r * r.transpose());
            prods.local() += cov;
            sums.local() += r;
            norms.local() += r.squaredNorm();
            send_item(i);
        }
    }
#pragma omp taskwait

    VectorNd sum = sums.combine();
    MatrixNNd prod = prods.combine();   
    double norm = norms.combine();

    const int N = num();
    local_sum() = sum;
    local_cov() = (prod - (sum * sum.transpose() / N)) / (N-1);
    local_norm() = norm;
}

void Sys::register_time(int i, double t)
{
    if (measure_perf) sample_time.at(i) += t;
}