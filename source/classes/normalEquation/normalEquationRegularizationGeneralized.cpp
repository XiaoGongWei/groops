/***********************************************/
/**
* @file normalEquationRegularizationGeneralized.cpp
*
* @brief Regularization with sum of symmetric matrices.
* @see NormalEquation
*
* @author Andreas Kvas
* @date 2010-02-10
*
*/
/***********************************************/

#include "base/import.h"
#include "config/config.h"
#include "files/fileMatrix.h"
#include "parallel/parallel.h"
#include "classes/normalEquation/normalEquation.h"
#include "classes/normalEquation/normalEquationRegularizationGeneralized.h"

/***********************************************/

NormalEquationRegularizationGeneralized::NormalEquationRegularizationGeneralized(Config &config)
{
  try
  {
    FileName fileNameBias;

    readConfig(config, "inputfilePartialCovarianceMatrix", fileNamesCovariance, Config::MUSTSET,  "",    "symmetric matrix (sum of all matrices must be positive definite)");
    readConfig(config, "inputfileBiasMatrix",              fileNameBias,        Config::OPTIONAL, "",    "bias vector (default: zero vector)");
    readConfig(config, "aprioriSigma",                     sigma2,              Config::MUSTSET,  "1.0", "apriori sigmas for initial iteration (default: 1.0)");
    readConfig(config, "startIndex",                       startIndex,          Config::DEFAULT,  "0",   "regularization of parameters starts at this index (counting from 0)");
    if(isCreateSchema(config)) return;

    if(sigma2.size() == 1)
      sigma2.resize(fileNamesCovariance.size(), sigma2.front());
    if(sigma2.size() != fileNamesCovariance.size())
      throw(Exception("Number of partial covariance matrices and apriori sigmas do not match ("+fileNamesCovariance.size()%"%i"s+" vs. "+ sigma2.size()%"%i"s+")."));
    for(Double &s2 : sigma2)
      s2 *= s2;

    if(Parallel::isMaster())
    {
      Matrix V;
      readFileMatrix(fileNamesCovariance.front(), V);
      paraCount = V.rows();

    }
    Parallel::broadCast(paraCount);

    if(!fileNameBias.empty())
     readFileMatrix(fileNameBias, bias);
    rhsCount = std::max(bias.columns(), static_cast<UInt>(1));
  }
  catch(std::exception &e)
  {
    GROOPS_RETHROW(e)
  }
}

/***********************************************/

void NormalEquationRegularizationGeneralized::init(MatrixDistributed &normals, UInt rhsCount)
{
  try
  {
    // adjust right hand side
    // ----------------------
    if(!bias.size())
      bias = Matrix(paraCount, rhsCount);
    if(bias.columns() != rhsCount)
      throw(Exception("Dimension error (right hand side count): "+bias.columns()%"%i != "s+rhsCount%"%i"s));
    this->rhsCount = bias.columns();

    startBlock = normals.index2block(startIndex);

    // index
    std::vector<UInt> index(paraCount);
    std::iota(index.begin(), index.end(), 0);

    // blockIndex
    std::vector<UInt> blockIndex = {0, normals.blockIndex(startBlock+1)-startIndex};
    for(UInt i=startBlock+1; blockIndex.back()<paraCount; i++)
      blockIndex.push_back(std::min(blockIndex.back() + normals.blockSize(i), paraCount));

    // calcRank
    auto calcRank = [&](UInt i, UInt k, UInt commSize) {return normals.getCalculateRank()(startBlock+i, startBlock+k, commSize);};

    for(const FileName &fileName : fileNamesCovariance)
    {
      MatrixDistributed Vi({0, paraCount}, normals.communicator()); // just one block
      if(Vi.isMyRank(0, 0))
      {
        readFileMatrix(fileName, Vi.N(0, 0));
        if((Vi.N(0, 0).rows() != paraCount) || (Vi.N(0, 0).getType() != Matrix::SYMMETRIC))
          throw(Exception("Dimension error of <"+fileName.str()+">"));
      }
      Vi.reorder(index, blockIndex, calcRank); // distribute blocks
      V.push_back(Vi);
    }

    Sigma.init(blockIndex, normals.communicator(), calcRank);
  }
  catch(std::exception &e)
  {
    GROOPS_RETHROW(e)
  }
}

/***********************************************/

Bool NormalEquationRegularizationGeneralized::addNormalEquation(UInt rhsNo, const const_MatrixSlice &x, const const_MatrixSlice &Wz,
                                                                MatrixDistributed &normals, Matrix &n, Vector &lPl, UInt &obsCount)
{
  try
  {
    // --- lambda function --------------------------
    auto symMatMult = [](const MatrixDistributed N, const_MatrixSliceRef &x) -> Matrix
    {
      Matrix y(x.rows(), x.columns());
      for(UInt i=0; i<N.blockCount(); i++)
        for(UInt k=i; k<N.blockCount(); k++)
          if(N.isMyRank(i, k))
          {
            matMult(1.0, N.N(i, k), x.row(N.blockIndex(k), N.blockSize(k)), y.row(N.blockIndex(i), N.blockSize(i)));
            if(i != k)
              matMult(1.0, N.N(i, k).trans(), x.row(N.blockIndex(i), N.blockSize(i)), y.row(N.blockIndex(k), N.blockSize(k)));
          }
      Parallel::reduceSum(y);
      return y;
    };
    // ----------------------------------------------

    UInt ready = 0;
    if(quadsum(x.slice(startIndex, rhsNo, paraCount, 1)) > 0.) // estimate sigmas
    {
      Matrix Se  = symMatMult(Sigma, bias.column(rhsNo) - x.slice(startIndex, rhsNo, paraCount, 1));
      Matrix SWz = symMatMult(Sigma, Wz);
      Parallel::broadCast(Se);
      Parallel::broadCast(SWz);

      for(UInt j=0; j<V.size(); j++)
      {
        Matrix VSe  = symMatMult(V.at(j), Se);
        Matrix VSWz = symMatMult(V.at(j), SWz);

        // trace(V*Sigma^-1)
        Double r = 0;
        for(UInt i=0; i<Sigma.blockCount(); i++)
        {
          for(UInt k=i; k<Sigma.blockCount(); k++)
            if(Sigma.isMyRank(i, k))
              r += 2 * inner(V.at(j).N(i,k), Sigma.N(i,k));
          if(Sigma.isMyRank(i, i))
            for(UInt z=0; z<Sigma.blockSize(i); z++)
              r -= V.at(j).N(i,i)(z,z) * Sigma.N(i,i)(z,z); // diagonal is accounted twice
        }
        Parallel::reduceSum(r);

        if(Parallel::isMaster())
        {
          const Double sigma2Old = sigma2.at(j);
          sigma2.at(j) *= inner(Se, VSe)/(r-inner(SWz, VSWz));
          ready = ready && (std::fabs(std::sqrt(sigma2.at(j))-std::sqrt(sigma2Old))/std::sqrt(sigma2.at(j)) < 0.01);
        }
      }
      Parallel::broadCast(sigma2);
      Parallel::broadCast(ready);
    }

    // accumulate covariance matrix
    Sigma.setNull();
    for(UInt i=0; i<Sigma.blockCount(); i++)
      for(UInt k=i; k<Sigma.blockCount(); k++)
        if(Sigma.isMyRank(i, k))
          for(UInt j=0; j<V.size(); j++)
            axpy(sigma2.at(j), V.at(j).N(i, k), Sigma.N(i, k));

    // invert for normal equations
    Sigma.cholesky(FALSE);
    Sigma.choleskyInverse(FALSE);
    Sigma.choleskyProduct(FALSE);

    // accumulate right hand side
    Matrix Pl = symMatMult(Sigma, bias);
    if(Parallel::isMaster())
    {
      axpy(1., Pl, n.row(startIndex, paraCount));
      for(UInt i=0; i<lPl.rows(); i++)
        lPl += inner(bias.column(i), Pl.column(i));
      obsCount += paraCount;
    }

    // accumulate normals
    for(UInt i=0; i<Sigma.blockCount(); i++)
      for(UInt k=i; k<Sigma.blockCount(); k++)
      {
        normals.setBlock(i, k);
        if(Sigma.isMyRank(i, k))
        {
          const UInt rowStart = startIndex + Sigma.blockIndex(i) - normals.blockIndex(startBlock+i);
          const UInt colStart = startIndex + Sigma.blockIndex(k) - normals.blockIndex(startBlock+k);
          axpy(1.0, Sigma.N(i, k), normals.N(startBlock+i, startBlock+k).slice(rowStart, colStart, Sigma.blockSize(i), Sigma.blockSize(k)));
        }
      }

    return ready;
  }
  catch(std::exception &e)
  {
    GROOPS_RETHROW(e)
  }
}

/***********************************************/

Vector NormalEquationRegularizationGeneralized::contribution(MatrixDistributed &Cov)
{
  try
  {
    Vector contrib(Cov.dimension());
    for(UInt i=0; i<Sigma.blockCount(); i++)
    {
      // diagonal block
      if(Sigma.isMyRank(i, i))
      {
        const UInt rowStart = startIndex + Sigma.blockIndex(i) - Cov.blockIndex(startBlock+i);
        const_MatrixSliceRef C = Cov.N(startBlock+i, startBlock+i).slice(rowStart, rowStart, Sigma.blockSize(i), Sigma.blockSize(i));

        for(UInt z=0; z<Sigma.blockSize(i); z++)
          contrib(startIndex+Sigma.blockIndex(i)+z) += inner(Sigma.N(i, i).slice(0, z, z, 1), C.slice(0, z, z, 1))
                                                    +  inner(Sigma.N(i, i).slice(z, z, 1, Sigma.blockSize(i)-z), C.slice(z, z, 1, Sigma.blockSize(i)-z));
      }
      // other blocks
      for(UInt k=i+1; k<Sigma.blockCount(); k++)
        if(Sigma.isMyRank(i, k))
        {
          const UInt rowStart = startIndex + Sigma.blockIndex(i) - Cov.blockIndex(startBlock+i);
          const UInt colStart = startIndex + Sigma.blockIndex(k) - Cov.blockIndex(startBlock+k);
          const_MatrixSliceRef C = Cov.N(startBlock+i, startBlock+k).slice(rowStart, colStart, Sigma.blockSize(i), Sigma.blockSize(k));

          for(UInt z=0; z<Sigma.blockSize(i); z++)
            contrib(startIndex+Sigma.blockIndex(i)+z) += inner(Sigma.N(i, k).row(z), C.row(z));
          for(UInt s=0; s<Sigma.blockSize(k); s++)
            contrib(startIndex+Sigma.blockIndex(k)+s) += inner(Sigma.N(i, k).column(s), C.column(s));
        }
    }
    Parallel::reduceSum(contrib);
    Parallel::broadCast(contrib);
    return contrib;
  }
  catch(std::exception &e)
  {
    GROOPS_RETHROW(e)
  }
}

/***********************************************/
