#include "HessianLearner.h"

#include "mkl.h"
#include "Utils.h"

#include <unordered_set>
#include <algorithm>
#include <iterator>
#include <iostream>

HessianLearner::HessianLearner()
    : Learner(), solver(MKL_DSS_ZERO_BASED_INDEXING +
        MKL_DSS_MSG_LVL_WARNING + MKL_DSS_TERM_LVL_ERROR + MKL_DSS_REFINEMENT_ON),
        include_Hf(false)
{
}

void HessianLearner::FinalizeCallback()
{
    rhs.resize(GetNumberOfAugmentedParameters());
    _x.resize(GetNumberOfAugmentedParameters(), 1.0);
    expx.resize(GetNumberOfParameters());

    Jg.Init(GetNumberOfParameters(), GetNumberOfConstraints(), Crow.data(), Ccol.data(), expx.data());
}

void HessianLearner::InitDss(bool reorder)
{
    const MKL_INT rows = GetNumberOfAugmentedParameters();
    const MKL_INT cols = GetNumberOfAugmentedParameters(), nnz = H.size();
    MKL_INT status;
    solver_opt = MKL_DSS_SYMMETRIC;
    auto solver_handle = solver.GetHandler();

    if ((status = dss_define_structure(solver_handle, solver_opt, Hrow.data(), rows, cols, Hcol.data(), nnz)) != MKL_DSS_SUCCESS)
    {
        throw LearnerError("Unable to define structure! Error code: ", status);
    }
    solver_opt = reorder ? MKL_DSS_GET_ORDER : MKL_DSS_MY_ORDER;
    perm.resize(rows);
    if (!reorder)
    {   // stick to original order
        for (MKL_INT i = 0; i < rows; ++i)
            perm[i] = i;
    }
    if ((status = dss_reorder(solver_handle, solver_opt, perm.data())) != MKL_DSS_SUCCESS)
    {
        throw LearnerError("Unable to find reorder! Error code: ", status);
    }
}

HessianLearner::~HessianLearner()
{
}

void HessianLearner::OptimizationStep(double eta, bool verbose)
{
    aux.resize(GetNumberOfAugmentedParameters());

    ComputeRhs();
    ComputeObjective();
    
    // H = 0
    cblas_dscal(H.size(), 0.0, H.data(), 1);

    if (include_Hf)
        ComputeHf();

    ComputeHg();
    
    if (verbose)
    {
        fputs("H:\n", stderr);
        PrintEq(stderr);
    }

    // sol = H \ rhs
    MKL_INT nRhs = 1, status;
    solver_opt = MKL_DSS_INDEFINITE;
    auto solver_handle = solver.GetHandler();
    if ((status = dss_factor_real(solver_handle, solver_opt, H.data())) != MKL_DSS_SUCCESS)
    {
        throw LearnerError("Unable to factor coefficient matrix! Error code: ", status);
    }

    solver_opt = MKL_DSS_REFINEMENT_ON;
    if ((status = dss_solve_real(solver_handle, solver_opt, rhs.data(), nRhs, aux.data())) != MKL_DSS_SUCCESS)
    {
        throw LearnerError("Unable to solve equation! Error code: ", status);
    }
    cblas_daxpy(aux.size(), -eta, aux.data(), 1, _x.data(), 1);
}

void HessianLearner::InitCallback(int flags)
{
    size_t i = 0;
    size_t place = 4;
    ProgressIndicator<size_t>(0, &i, 1,
        "\rInitialize %5X ",
        [&]()
    {
        const size_t done = 0xE;
        const size_t skipped = 0xF;
        
        if (flags & 1)
        {
            _x.assign(GetNumberOfParameters(), 0.0);
            _x.resize(GetNumberOfParameters() + GetNumberOfConstraints(), 1.0);
            i += (done << (4 * place));
        }else
            i += (skipped << (4 * place));
        --place;
        
        if (flags & 2)
        {
            Renormalize();
            i += (done << (4 * place));
        }else
            i += (skipped << (4 * place));
        --place;
        
        if (flags & 4)
        {
            InitSlackVariables();
            i += (done << (4 * place));
        }else
            i += (skipped << (4 * place));
        --place;
        
        include_Hf = flags & 8;
        AssembleH(include_Hf);
        i += ((include_Hf ? done : skipped) << (4 * place));
        --place;
        
        InitDss(flags & 16);
        i += (((flags & 16) ? done : skipped) << (4 * place));
    });
}

size_t HessianLearner::GetNumberOfAugmentedParameters() const
{
    return GetNumberOfParameters() + GetNumberOfConstraints();
}

double HessianLearner::DeNormalizedFactor() const
{
    const auto n = GetNumberOfParameters(), k = GetNumberOfConstraints();
    return std::abs(rhs[n + cblas_idamax(k, rhs.data() + n, 1)]);
}

double HessianLearner::GradientError() const
{
    return std::abs(rhs[cblas_idamax(GetNumberOfParameters(), rhs.data(), 1)]);
}

size_t HessianLearner::GetNnzHessian() const
{
    return Hcol.size();
}

double HessianLearner::GetHessianFillRatio() const
{
    return (double)Hcol.size() / (Hrow.size() - 1);
}

double HessianLearner::ComputeLogDetHessian()
{
    // Hf is not zero, but hasn't been included
    if (!include_Hf && !unique_path)
        AssembleH(true);

    const MKL_INT n = GetNumberOfParameters();
    std::vector<MKL_INT> newHcol; newHcol.reserve(n); // at least
     // modify the nnz structure of H, you have to just erase the last element from each row
    for (MKL_INT i = 0; i < n; ++i)
    {
        newHcol.insert(newHcol.end(), &Hcol[Hrow[i]], &Hcol[Hrow[i + 1] - 1]);
        Hrow[i] -= i;
    }
    Hrow.resize(n + 1);
    Hrow.back() = Hrow[n - 1] + 1;
    std::swap(newHcol, Hcol);
    H.resize(Hrow.back());
    const auto nnz = H.size();
    // H = 0
    cblas_dscal(nnz, 0, H.data(), 1);
    
    // fill in the values, similar to Hf
    ComputeExpX(); // these are the actual variables of the current Hessian
    auto& x = expx;
   
    ComputeGrad();

    ComputeHf();

    for (MKL_INT j = 0; j < n; ++j)
    {
        MKL_INT k = Hrow[j];
        H[k] -= rhs[j];
        for (; k < Hrow[j + 1]; ++k)
        {
            H[k] /= x[j] * x[Hcol[k]];
        }
    }
   
    return RealSymmetricLogDet(Hrow.data(), n, Hcol.data(), nnz, H.data(), false);
}

void HessianLearner::PrintH(FILE* f) const
{
    PrintCsrMtx(f, H, Hrow, Hcol);
}

void HessianLearner::PrintEq(FILE* f) const
{
    PrintCsrMtx(f, H, Hrow, Hcol, rhs);
}

std::vector<double> HessianLearner::GetOptimizationInfo()
{
    std::vector<double> result(6);
    result[0] = GetKLDistance();

    result[1] = GradientError();
    result[2] = DeNormalizedFactor();
    error = std::max(result[1], result[2]);

    MKL_INT status;
    solver_opt = MKL_DSS_DEFAULTS;
    double* ret_values = result.data() + 3;
    auto solver_handle = solver.GetHandler();

    if ((status = dss_statistics(solver_handle, solver_opt, "Inertia", ret_values)) != MKL_DSS_SUCCESS)
    {
        throw LearnerError("Unable to get inertia! Error code: ", status);
    }
    return result;
}

std::vector<double> HessianLearner::GetOptimizationResult(bool verbose)
{
    ComputeModeledProbs();
    ComputeObjective();

    const auto logdetHessian = ComputeLogDetHessian();

    std::cerr << "Hessian:\n\trows: " << GetNumberOfParameters() <<
        "\n\tnnz: " << GetNnzHessian() <<
        "\n\tfill: " << GetHessianFillRatio() << std::endl;
    if (verbose)
        PrintH(stderr);
    
    return std::vector<double>({
        GetKLDistance(),
        mxlogx(GetCommonSupport()),
        LogModelVolume(),
        LogAuxiliaryVolume(),
        logdetHessian,
        LogDetAuxiliaryHessian(),
        (double)(GetNumberOfParameters() - GetNumberOfConstraints()),
        (double)std::max<MKL_INT>(0, GetNumberOfAuxParameters() - 1)
        });
}

std::string HessianLearner::GetOptimizationHeader()
{
    return "KL\tgraderr\tnormerr\t   +\t   -\t   0";
}

bool HessianLearner::HaltCondition(double tol)
{
    return error <= tol;
}

void HessianLearner::AssembleH(bool Hf)
{
    std::vector<std::pair<MKL_INT, MKL_INT>> indices;
    std::vector<std::pair<MKL_INT, double>> equivocal_intersect;
    equivocal_str_indices.clear();

    // gather nnz indexes
    if (Hf && !unique_path)
    {
        struct Comparer
        {
            Comparer(MKL_INT* c, double* d)
                : cols(c), data(d)
            {
            }
            bool less(const std::pair<MKL_INT, double>& i, const MKL_INT& j)const
            {
                return (i.first < cols[j]) ||
                    (!(i.first < cols[j]) && (i.second < data[j]));
            }
            bool greater(const std::pair<MKL_INT, double>& i, const MKL_INT& j)const
            {
                return (i.first > cols[j]) ||
                    (!(i.first > cols[j]) && (i.second > data[j]));
            }
            MKL_INT* cols;
            double* data;
        } const comp(Pcol.data(), Pdata.data());

        for (size_t str_idx = 0; str_idx < Mrow.size() - 1; ++str_idx)
        {
            if (Mrow[str_idx] + 1 < Mrow[str_idx + 1])
            {   // equivocal str
                auto path_idx = Mrow[str_idx];
                auto end_path_idx = Mrow[str_idx + 1];

                equivocal_str_indices.emplace_back();
                equivocal_str_indices.back().first = str_idx;
                auto& equivocal_indices = equivocal_str_indices.back().second;

                equivocal_indices.assign(Pcol.begin() + Prow[path_idx], Pcol.begin() + Prow[path_idx + 1]);
                equivocal_intersect.clear();
                std::transform(Pcol.begin() + Prow[path_idx], Pcol.begin() + Prow[path_idx + 1],
                        Pdata.begin() + Prow[path_idx], std::back_inserter(equivocal_intersect),
                    [](MKL_INT a, double b) {return std::pair<MKL_INT, double>(a, b); });

                for (++path_idx; path_idx < end_path_idx; ++path_idx)
                {
                    const auto first_col_idx = Prow[path_idx];
                    const auto end_col_idx = Prow[path_idx + 1];

                    Union(equivocal_indices, Pcol.begin() + first_col_idx, Pcol.begin() + end_col_idx);
                    Intersect(equivocal_intersect, first_col_idx, end_col_idx, comp);
                }

                Subtract(equivocal_indices, make_first_iterator(equivocal_intersect.begin()), make_first_iterator(equivocal_intersect.end()));

                //! full sub-matrix for this string
                for (auto i = equivocal_indices.begin(); i != equivocal_indices.end(); ++i)
                {
                    for (auto j = i; j != equivocal_indices.end(); ++j)
                        SortedInsert(indices, std::make_pair(*i, *j));
                }
            }
        }
    }

    // convert to csr format
    const MKL_INT n = GetNumberOfParameters();
    const auto k = GetNumberOfConstraints();
    H.clear(); Hrow.clear(); Hcol.clear();
    Hrow.reserve(n + k + 1);
    Hcol.reserve(2 * n + k); // lower bound
    Hrow.emplace_back(0);

    MKL_INT row = 0;
    if ((indices.empty() || indices.front().first > 0) && n > 0)
        Hcol.emplace_back(0);
    for (const auto& ij : indices)
    {
        const auto& i = ij.first;
        if (i > row)
        {
            for (MKL_INT r = row; r < ij.first; ++r)
            {
                // J_g
                Hcol.emplace_back(GetNumberOfParameters() + Ccol[r]);
                // end of row
                Hrow.emplace_back(Hcol.size());

                if (r + 1 < ij.first)
                    Hcol.emplace_back(r + 1); // diagonal, if missing

            }
            row = ij.first;
        }
        Hcol.emplace_back(ij.second);
    }
    for (MKL_INT r = row; r < n; ++r)
    {
        // J_g
        Hcol.emplace_back(GetNumberOfParameters() + Ccol[r]);
        // end of row
        Hrow.emplace_back(Hcol.size());
        if (r + 1 < n)
            Hcol.emplace_back(r + 1); // diagonal, if missing
    }

    // zeros at the bottom-right corner
    for (size_t i = n; i < n + k; ++i)
    {
        Hcol.emplace_back(i);
        Hrow.emplace_back(Hcol.size());
    }

    H.resize(Hcol.size(), 0.0);
}

void HessianLearner::ComputeHf()
{
    std::vector<double> grad_qi_qi;
    std::vector<double> H_qi;

    for (const auto& equivocal_str : equivocal_str_indices)
    {
        auto str_idx = equivocal_str.first;
        const auto& equivocal_indexes = equivocal_str.second;
        auto pi = p[str_idx];
        grad_qi_qi.assign(equivocal_indexes.size(), 0.0);
        H_qi.assign(equivocal_indexes.size() * (equivocal_indexes.size()-1) / 2, 0.0);

        for (auto l = Mrow[str_idx]; l < Mrow[str_idx + 1]; ++l)
        {
            auto pl = Prow[l];
            MKL_INT j = 0;
            while (j < equivocal_indexes.size() && pl < Prow[l + 1])
            {
                // https://stackoverflow.com/a/4609795/3583290
                switch ((equivocal_indexes[j] < Pcol[pl]) - (Pcol[pl] < equivocal_indexes[j]))
                {
                case -1: ++pl; break;
                case  1: ++j; break;
                default: 
                    grad_qi_qi[j] += Pdata[pl] * relative_path_probs[l];
                    ++j;
                    ++pl;
                }
            }
        }

        for (MKL_INT j = 0; j < equivocal_indexes.size(); ++j)
        {
            for (MKL_INT k = j; k < equivocal_indexes.size(); ++k)
            {
                double Hjk = 0.0;
                for (auto l = Mrow[str_idx]; l < Mrow[str_idx + 1]; ++l)
                {
                    // TODO this could be faster!
                    Hjk -= GetCoord2(Prow, Pcol, Pdata, l, equivocal_indexes[j]) *
                           GetCoord2(Prow, Pcol, Pdata, l, equivocal_indexes[k]) *
                           relative_path_probs[l];
                }
                Hjk += grad_qi_qi[j] * grad_qi_qi[k];
                GetCoord(Hrow, Hcol, H, equivocal_indexes[j], equivocal_indexes[k]) += pi * Hjk;
            }
        }
    }
}

void HessianLearner::ComputeExpX()
{
    vdExp(GetNumberOfParameters(), _x.data(), expx.data());
}

void HessianLearner::InitSlackVariables()
{
    const MKL_INT n = GetNumberOfParameters(), k = GetNumberOfConstraints();
    std::vector<double> sumexp2x(k);
    // this is going to contain the pseudo inverse of J
    auto& PInvJ = aux;
    PInvJ.resize(n);

    ComputeExpX();
    ComputeGrad();
    
    {   //PInvJ <- exp(2*x)

        // PInvJ <- _x
        cblas_dcopy(n, _x.data(), 1, PInvJ.data(), 1);
        // PInvJ *= 2
        cblas_dscal(n, 2.0, PInvJ.data(), 1);
        // PInvJ <- exp(PInvJ)
        vdExp(n, PInvJ.data(), PInvJ.data());
    }
    {
        // sumexp2x <- C^t.PInvJ
        C.dot(PInvJ.data(), sumexp2x.data(), SPARSE_OPERATION_TRANSPOSE);

        // PInvJ <- C*sumexp2x
        C.dot(sumexp2x.data(), PInvJ.data(), SPARSE_OPERATION_NON_TRANSPOSE);
    }
    // PInvJ <- exp(x) / PInvJ
    vdDiv(n, expx.data(), PInvJ.data(), PInvJ.data());

    // -PInvJ.gradf
    SparseMtxHandle PInv_mat(n, k, Crow.data(), Ccol.data(), PInvJ.data());
    PInv_mat.dot(rhs.data(), _x.data() + n, SPARSE_OPERATION_TRANSPOSE, -1.0, 0.0);
}

void HessianLearner::ComputeGrad()
{
    if (grad_aux.empty())
    {   // initialize
        if (unique_path)
        {   // gradient is constant: -P^t.p
            grad_aux.resize(GetNumberOfParameters());
            P.dot(p.data(), grad_aux.data(), SPARSE_OPERATION_TRANSPOSE, -1.0, 0.0);
        }
        else
        {   // a useful quantity is precomputed: -M^t.p
            grad_aux.resize(GetNumberOfPaths());
            M.dot(p.data(), grad_aux.data(), SPARSE_OPERATION_TRANSPOSE, -1.0, 0.0);
        }
    }

    ComputeModeledProbs();
    if (unique_path)
    {
        // rhs <- grad_aux
        cblas_dcopy(GetNumberOfParameters(), grad_aux.data(), 1, rhs.data(), 1);
    }
    else
    {
        aux.resize(GetNumberOfPaths());

        // aux <- (-M^t.p)*relative_path_probs
        vdMul(GetNumberOfPaths(), relative_path_probs.data(), grad_aux.data(), aux.data());

        // rhs <- P^t.aux
        P.dot(aux.data(), rhs.data(), SPARSE_OPERATION_TRANSPOSE);
    }
}

void HessianLearner::ComputeG()
{
    const MKL_INT n = GetNumberOfParameters();
    const MKL_INT k = GetNumberOfConstraints();
    // g <- C^t.exp(x)
    C.dot(expx.data(), rhs.data() + n, SPARSE_OPERATION_TRANSPOSE);
    
    // g -= 1
    cblas_daxpy(k, -1.0, ones.data(), 1, rhs.data() + n, 1);
}

void HessianLearner::ComputeRhs()
{
    ComputeExpX();
    ComputeGrad();
    ComputeG();

    // J_g.l = expx[C].lambda
    // rhs[:n] += J_g.l
    const auto n = GetNumberOfParameters();
    Jg.dot(_x.data() + n, rhs.data(), SPARSE_OPERATION_NON_TRANSPOSE, 1.0, 1.0);
}

void HessianLearner::ComputeHg()
{
    const MKL_INT n = GetNumberOfParameters(), k = GetNumberOfConstraints();
    const double* lambda = _x.data() + n;

    if (n > 0)
        H[0] += expx[0]*lambda[0];
    for (MKL_INT i = 0; i < n - 1; ++i)
    {
        const auto r = Hrow[i + 1];
        H[r - 1] += expx[i];
        H[r] += expx[i + 1] * lambda[Ccol[i + 1]];
    }
    if (n > 0)
    {
        H[H.size() - k - 1] += expx.back();
    }
}
