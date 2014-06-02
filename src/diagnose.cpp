#include "rpf.h"

#include <iostream>

struct ifaGroup {
	std::vector<double*> spec;
	int numItems;
	int maxParam;
	double *param;
	int numSpecific;
	int maxAbilities;
	double *mean;
	double *cov;

	// TODO:
	// scores
	// data

	void import(SEXP Rlist);
	double *getItemParam(int ix) { return param + maxParam * ix; }
};

void ifaGroup::import(SEXP Rlist)
{
	SEXP argNames;
	Rf_protect(argNames = Rf_getAttrib(Rlist, R_NamesSymbol));

	int mlen = 0;
	int nrow, ncol;
	for (int ax=0; ax < Rf_length(Rlist); ++ax) {
		const char *key = R_CHAR(STRING_ELT(argNames, ax));
		SEXP slotValue = VECTOR_ELT(Rlist, ax);
		if (strEQ(key, "spec")) {
			for (int sx=0; sx < Rf_length(slotValue); ++sx) {
				SEXP model = VECTOR_ELT(slotValue, sx);
				if (!OBJECT(model)) {
					Rf_error("Item models must inherit rpf.base");
				}
				SEXP Rspec;
				Rf_protect(Rspec = R_do_slot(model, Rf_install("spec")));
				spec.push_back(REAL(Rspec));
			}
		} else if (strEQ(key, "param")) {
			param = REAL(slotValue);
			getMatrixDims(slotValue, &maxParam, &numItems);
		} else if (strEQ(key, "mean")) {
			mlen = Rf_length(slotValue);
			mean = REAL(slotValue);
		} else if (strEQ(key, "cov")) {
			getMatrixDims(slotValue, &nrow, &ncol);
			if (nrow != ncol) Rf_error("cov must be a square matrix (not %dx%d)", nrow, ncol);
			cov = REAL(slotValue);
		} else {
			// ignore
		}
	}
	if (mlen != nrow) Rf_error("Mean length %d does not match cov size %d", mlen, nrow);
	if (numItems != spec.size()) {
		Rf_error("param implies %d items but spec is length %d",
			 numItems, spec.size());
	}

	// detect two-tier covariance structure
	std::vector<int> orthogonal;
	if (mlen >= 3) {
		Eigen::Map<Eigen::MatrixXd> Ecov(cov, mlen, mlen);
		Eigen::Matrix<Eigen::DenseIndex, Eigen::Dynamic, 1> numCov((Ecov.array() != 0.0).matrix().colwise().count());
		std::vector<int> candidate;
		for (int fx=0; fx < numCov.rows(); ++fx) {
			if (numCov(fx) == 1) candidate.push_back(fx);
		}
		if (candidate.size() > 1) {
			std::vector<bool> mask(numItems);
			for (int cx=candidate.size() - 1; cx >= 0; --cx) {
				std::vector<bool> loading(numItems);
				for (int ix=0; ix < numItems; ++ix) {
					loading[ix] = param[ix * maxParam + candidate[cx]] != 0;
				}
				std::vector<bool> overlap(loading.size());
				std::transform(loading.begin(), loading.end(),
					       mask.begin(), overlap.begin(),
					       std::logical_and<bool>());
				if (std::find(overlap.begin(), overlap.end(), true) == overlap.end()) {
					std::transform(loading.begin(), loading.end(),
						       mask.begin(), mask.begin(),
						       std::logical_or<bool>());
					orthogonal.push_back(candidate[cx]);
				}
			}
		}
	}
	std::reverse(orthogonal.begin(), orthogonal.end());
	if (orthogonal.size() && orthogonal[0] != mlen - orthogonal.size()) {
		Rf_error("Independent factors must be given after dense factors");
	}

	maxAbilities = mlen;
	numSpecific = orthogonal.size();
}

class ssEAP {
	int lastItem;
public:
	ifaGroup grp;
	ba81NormalQuad quad;
	int maxScore;
	std::vector<int> items;
	std::vector<int> itemOutcomes;
	Eigen::MatrixXd slCur;
	Eigen::MatrixXd slPrev;

	void setup(SEXP grp, double qwidth, int qpts);
	void setLastItem(int which);
	void tpbw1995();
};

#include <iostream> //TODO remove

void ssEAP::setup(SEXP robj, double qwidth, int qpts)
{
	lastItem = 0;

	grp.import(robj);

	Eigen::Map<Eigen::MatrixXd> fullCov(grp.cov, grp.maxAbilities, grp.maxAbilities);
	int dense = grp.maxAbilities - grp.numSpecific;
	Eigen::MatrixXd priCov = fullCov.block(0, 0, dense, dense);
	Eigen::VectorXd sVar = fullCov.diagonal().tail(grp.numSpecific);
	quad.setup(qwidth, qpts, grp.mean, priCov, sVar);

	maxScore = 0;
	itemOutcomes.reserve(grp.numItems);
	for (int cx = 0; cx < grp.numItems; cx++) {
		const double *spec = grp.spec[cx];
		int no = spec[RPF_ISpecOutcomes];
		itemOutcomes.push_back(no);
		maxScore += no - 1;
	}
}

void ssEAP::setLastItem(int which)
{
	lastItem = which;
}

void ssEAP::tpbw1995()
{
	items.reserve(grp.numItems);
	for (int ix=0; ix < grp.numItems; ++ix) if (ix != lastItem) items.push_back(ix);
	items.push_back(lastItem);

	slCur.resize(quad.totalQuadPoints, 1+maxScore);
	int curMax = 0;
	int item0 = items[0];
	{
		const double *spec = grp.spec[item0];
		int id = spec[RPF_ISpecID];
		const int dims = spec[RPF_ISpecDims];
		double *iparam = grp.getItemParam(item0);
		int outcomes = itemOutcomes[item0];
		for (int qx=0; qx < quad.totalQuadPoints; ++qx) {
			double oprob[outcomes];
			double *where = quad.wherePrep.data() + qx * quad.maxDims;
			double ptheta[dims];
			for (int dx=0; dx < dims; dx++) {
				ptheta[dx] = where[std::min(dx, quad.maxDims-1)];
			}
			librpf_model[id].prob(spec, iparam, ptheta, oprob);
			for (int ox=0; ox < outcomes; ++ox) {
				slCur(qx, ox) = oprob[ox];
			}
		}
		curMax += outcomes - 1;
	}

	slPrev.resize(quad.totalQuadPoints, 1+maxScore);

	for (int curItem=1; curItem < int(items.size()); ++curItem) {
		int ix = items[curItem];
		slCur.swap(slPrev);
		const double *spec = grp.spec[ix];
		int id = spec[RPF_ISpecID];
		const int dims = spec[RPF_ISpecDims];
		double *iparam = grp.getItemParam(ix);
		int outcomes = itemOutcomes[ix];
		slCur.topLeftCorner(slCur.rows(), curMax + outcomes).setZero();
		for (int qx=0; qx < quad.totalQuadPoints; ++qx) {
			double oprob[outcomes];
			double *where = quad.wherePrep.data() + qx * quad.maxDims;
			double ptheta[dims];
			for (int dx=0; dx < dims; dx++) {
				ptheta[dx] = where[std::min(dx, quad.maxDims-1)];
			}
			librpf_model[id].prob(spec, iparam, ptheta, oprob);
			for (int cx=0; cx <= curMax; cx++) {
				for (int ox=0; ox < outcomes; ox++) {
					slCur(qx, cx + ox) += slPrev(qx, cx) * oprob[ox];
				}
			}
		}
		curMax += outcomes - 1;
	}
}

SEXP ot2000_wrapper(SEXP robj, SEXP Ritem, SEXP Rwidth, SEXP Rpts, SEXP Ralter)
{
	omxManageProtectInsanity mpi;

	double qwidth = Rf_asReal(Rwidth);
	int qpts = Rf_asInteger(Rpts);
	int interest = Rf_asInteger(Ritem) - 1;

	ssEAP myeap;
	myeap.setup(robj, qwidth, qpts);
	myeap.setLastItem(interest);
	myeap.tpbw1995();

	int outcomes = myeap.itemOutcomes[interest];

	ifaGroup &grp = myeap.grp;
	ba81NormalQuad &quad = myeap.quad;
	Eigen::MatrixXd iProb(quad.totalQuadPoints, outcomes);

	{
		const double *spec = grp.spec[interest];
		int id = spec[RPF_ISpecID];
		const int dims = spec[RPF_ISpecDims];
		double *iparam = grp.getItemParam(interest);
		for (int qx=0; qx < quad.totalQuadPoints; ++qx) {
			double oprob[outcomes];
			double *where = quad.wherePrep.data() + qx * quad.maxDims;
			double ptheta[dims];
			for (int dx=0; dx < dims; dx++) {
				ptheta[dx] = where[std::min(dx, quad.maxDims-1)];
			}
			librpf_model[id].prob(spec, iparam, ptheta, oprob);
			for (int ox=0; ox < outcomes; ox++) {
				iProb(qx, ox) = oprob[ox];
			}
		}
	}

	if (Rf_asLogical(Ralter)) {
		Eigen::MatrixXd &slCur = myeap.slCur;
		Eigen::MatrixXd &slPrev = myeap.slPrev;

		Eigen::Map<Eigen::VectorXd> areaCol(quad.priQarea.data(), quad.priQarea.size());
		slCur.array().colwise() *= areaCol.array();
		slPrev.array().colwise() *= areaCol.array();

		Eigen::VectorXd ssProb(1+myeap.maxScore);
		ssProb = slCur.array().colwise().sum();

		int prevMaxScore = myeap.maxScore - (outcomes-1);
		SEXP Rexpected;
		Rf_protect(Rexpected = Rf_allocMatrix(REALSXP, 1+myeap.maxScore, outcomes));
		double *out = REAL(Rexpected);
		memset(out, 0, sizeof(double) * (1+myeap.maxScore) * outcomes);

		for (int startScore=0; startScore+outcomes <= 1+myeap.maxScore; ++startScore) {
			Eigen::MatrixXd numerDen(quad.totalQuadPoints, outcomes);
			numerDen = iProb.array().colwise() * slPrev.col(startScore).array();
			Eigen::VectorXd numer(outcomes);
			numer = numerDen.colwise().sum();
			for (int ox=0; ox < outcomes; ++ox) {
				out[ox * (1+myeap.maxScore) + (startScore+ox)] = numer(ox) / ssProb(startScore + ox);
			}
		}

		return Rexpected;
	} else {
		Eigen::MatrixXd &slPrev = myeap.slPrev;
	
		Eigen::Map<Eigen::VectorXd> areaCol(quad.priQarea.data(), quad.priQarea.size());
		slPrev.array().colwise() *= areaCol.array();

		Eigen::VectorXd ssProb(1+myeap.maxScore);
		ssProb = slPrev.array().colwise().sum();

		int prevMaxScore = myeap.maxScore - (outcomes-1);
		SEXP Rexpected;
		Rf_protect(Rexpected = Rf_allocMatrix(REALSXP, prevMaxScore + 1, outcomes));
		double *out = REAL(Rexpected);

		for (int startScore=0; startScore <= prevMaxScore; ++startScore) {
			Eigen::MatrixXd numerDen(quad.totalQuadPoints, outcomes);
			numerDen = iProb.array().colwise() * slPrev.col(startScore).array();
			Eigen::VectorXd numer(outcomes);
			numer = numerDen.colwise().sum();
			for (int ox=0; ox < outcomes; ++ox) {
				out[ox * (prevMaxScore+1) + startScore] = numer(ox) / ssProb(startScore);
			}
		}

		return Rexpected;
	}
}

SEXP sumscoreEAP(SEXP robj, SEXP Rwidth, SEXP Rpts)
{
	omxManageProtectInsanity mpi;

	double qwidth = Rf_asReal(Rwidth);
	int qpts = Rf_asInteger(Rpts);

	ssEAP myeap;
	myeap.setup(robj, qwidth, qpts);
	myeap.tpbw1995();

	ba81NormalQuad &quad = myeap.quad;
	Eigen::MatrixXd &slCur = myeap.slCur;

	if (quad.numSpecific == 0) {
		Eigen::Map<Eigen::VectorXd> areaCol(quad.priQarea.data(), quad.priQarea.size());
		slCur.array().colwise() *= areaCol.array();
	} else {
		Eigen::VectorXd den(quad.totalQuadPoints);
		for (int qloc=0, qx=0; qx < quad.totalPrimaryPoints; ++qx) {
			for (int sx=0; sx < quad.quadGridSize; ++sx) {
				den(qloc) = quad.priQarea[qx] * quad.speQarea[sx * quad.numSpecific];
				++qloc;
			}
		}
		slCur.array().colwise() *= den.array();
	}

	int curMax = myeap.maxScore;

	Eigen::VectorXd ssProb(1+curMax);
	ssProb = slCur.array().colwise().sum();

	int perScore = quad.maxAbilities + triangleLoc1(quad.maxAbilities);
	SEXP Rout;
	Rf_protect(Rout = Rf_allocMatrix(REALSXP, 1+curMax, 1+perScore));
	double *out = REAL(Rout);
	memcpy(out, ssProb.data(), sizeof(double) * (1+curMax));
	for (int cx=0; cx <= curMax; cx++) {
		std::vector<double> pad(perScore);
		if (quad.numSpecific == 0) {
			quad.EAP(&slCur.coeffRef(0, cx), 1/ssProb[cx], pad.data());
		} else {
			Eigen::MatrixXd weight(quad.numSpecific, quad.totalQuadPoints);
			weight.array().rowwise() = slCur.array().col(cx).transpose();
			quad.EAP(weight.data(), 1/ssProb[cx], pad.data());
		}
		for (int sx=0; sx < perScore; ++sx) {
			out[(1+sx) * (curMax+1) + cx] = pad[sx];
		}
	}

	// add dimnames TODO
	return Rout;
}

static void
decodeLocation(long qx, const int dims, const int gridsize,
	       const double *pts, double *out)
{
  for (int dx=0; dx < dims; dx++) {
    out[dx] = pts[qx % gridsize];
    qx = qx / gridsize;
  }
}

static void
pda(const double *ar, int rows, int cols) {
	for (int rx=0; rx < rows; rx++) {
		for (int cx=0; cx < cols; cx++) {
			Rprintf("%.6g ", ar[cx * rows + rx]);
		}
		Rprintf("\n");
	}

}

static const double KANG_CHEN_MIN_EXPECTED = 1.0;  // customizable parameter?

static void find_smallcol(int rows, int cols, double *expected, int rx,
			  int *goodcol, int *smallcol)
{
  *goodcol = 0;
  *smallcol = -1;
  double smallest = -1;
  for (int cx=0; cx < cols; cx++) {
    double cell = expected[cx * rows + rx];
    if (cell != 0 && cell <= KANG_CHEN_MIN_EXPECTED) {
      if (smallest == -1 || smallest > cell) {
	smallest = cell;
	*smallcol = cx;
      }
    } else {
      *goodcol += 1;
    }
  }
}

static int kang_chen_2007_collapse1(int rows, int cols, int *observed, double *expected, int rx)
{
  int collapsed = 0;
  int smallcol;
  int goodcol;
  find_smallcol(rows, cols, expected, rx, &goodcol, &smallcol);
  if (smallcol == -1) return collapsed;

  if (goodcol > cols/2) {
    // merge left or right
    while (1) {
      double biggest = 0;
      int bigcol = -1;
      if (smallcol - 1 >= 0) {
	double cell = expected[(smallcol-1)*rows + rx];
	if (biggest < cell) {
	  biggest = cell;
	  bigcol = smallcol-1;
	}
      }
      if (smallcol + 1 < cols) {
	double cell = expected[(smallcol+1)*rows + rx];
	if (biggest < cell) {
	  biggest = cell;
	  bigcol = smallcol+1;
	}
      }
      if (bigcol==-1) Rf_error("Confused");
      //Rprintf("collapse col %d to %d on row %d\n", smallcol, bigcol, rx);
      expected[bigcol*rows + rx] += expected[smallcol*rows + rx];
      observed[bigcol*rows + rx] += observed[smallcol*rows + rx];
      expected[smallcol*rows + rx] = NA_REAL;
      observed[smallcol*rows + rx] = NA_REAL;
      ++collapsed;

      find_smallcol(rows, cols, expected, rx, &goodcol, &smallcol);
      if (smallcol == -1) break;
    }
  } else {
    // merge up or down
    int bigrow;
    if (rx < rows/2) {
      bigrow = rx+1;
    } else {
      bigrow = rx-1;
    }
    for (int cx=0; cx < cols; cx++) {
      double e1 = expected[cx*rows + rx];
      if (e1 == 0 || e1 >= KANG_CHEN_MIN_EXPECTED)
	continue;
      //Rprintf("collapse row %d to %d at col %d\n", rx, bigrow, cx);
      expected[cx*rows + bigrow] += expected[cx*rows + rx];
      observed[cx*rows + bigrow] += observed[cx*rows + rx];
      expected[cx*rows + rx] = NA_REAL;
      observed[cx*rows + rx] = NA_REAL;
      ++collapsed;
    }
  }
  return collapsed;
}

/*
 * This assumes that the expected counts are greater than 1 in the
 * middle of the table. This may be suboptimal for large tables with
 * isolated regions of low expected counts (is it possible?). It may
 * be necessary to search the whole table at every step for the lowest
 * expected count and fix them in that order instead of assuming
 * anything about where the low expected counts will appear.
 */
static int kang_chen_2007_collapse(int rows, int cols, int *observed, double *expected)
{
  //Rprintf("kang chen 2007\n");
  //pda(expected, rows, cols);
  int collapsed = 0;
  for (int rx=0; rx <= rows/2; rx++) {
    collapsed += kang_chen_2007_collapse1(rows, cols, observed, expected, rx);
    if (rx != ((rows-1) -rx)) {
      collapsed += kang_chen_2007_collapse1(rows, cols, observed, expected, (rows-1) -rx);
    }
  }
  for (int rx=0; rx < rows; rx++) {
    for (int cx=0; cx < cols; cx++) {
      double cell = expected[cx * rows + rx];
      if (cell != 0 && cell <= KANG_CHEN_MIN_EXPECTED) {
	pda(expected, rows, cols);
	Rf_error("Failed to collapse cells");
      }
    }
  }
  return collapsed;
}

SEXP kang_chen_2007_wrapper(SEXP r_observed_orig, SEXP r_expected_orig)
{
  int rows, cols;
  getMatrixDims(r_expected_orig, &rows, &cols);

  {
    int orows, ocols;
    getMatrixDims(r_observed_orig, &orows, &ocols);
    if (rows != orows || cols != ocols)
	    Rf_error("Observed %dx%d and expected %dx%d matrices must have same dimensions",
		     orows, ocols, rows, cols);
  }

  SEXP r_observed, r_expected;
  Rf_protect(r_observed = Rf_allocMatrix(INTSXP, rows, cols));
  Rf_protect(r_expected = Rf_allocMatrix(REALSXP, rows, cols));

  int *observed = INTEGER(r_observed);
  double *expected = REAL(r_expected);
  memcpy(observed, INTEGER(r_observed_orig), sizeof(int) * rows * cols);
  memcpy(expected, REAL(r_expected_orig), sizeof(double) * rows * cols);

  int collapsed = kang_chen_2007_collapse(rows, cols, observed, expected);

  const int returnCount = 3;
  SEXP names, ans;
  Rf_protect(names = Rf_allocVector(STRSXP, returnCount));
  Rf_protect(ans = Rf_allocVector(VECSXP, returnCount));

  int ansC = -1;
  SET_STRING_ELT(names, ++ansC, Rf_mkChar("O"));
  SET_VECTOR_ELT(ans,   ansC, r_observed);
  SET_STRING_ELT(names, ++ansC, Rf_mkChar("E"));
  SET_VECTOR_ELT(ans,   ansC, r_expected);
  SET_STRING_ELT(names, ++ansC, Rf_mkChar("collapsed"));
  SET_VECTOR_ELT(ans,   ansC, Rf_ScalarInteger(collapsed));
  if (ansC != returnCount-1) Rf_error("Memory corruption");

  Rf_namesgets(ans, names);
  UNPROTECT(4);

  return ans;
}

SEXP sumscore_observed(SEXP r_high, SEXP r_data, SEXP r_interest, SEXP r_outcomes, SEXP Ralter)
{
	bool alter = Rf_asLogical(Ralter);
  int data_rows = Rf_length(VECTOR_ELT(r_data, 0));
  int data_cols = Rf_length(r_data);

  int interest = Rf_asInteger(r_interest);
  int outcomes = Rf_asInteger(r_outcomes);
  int high;
  if (!alter) {
	  high = Rf_asInteger(r_high) - (outcomes - 1);
  } else {
	  high = Rf_asInteger(r_high);
  }

  if (interest < 1 || interest > data_cols)
    Rf_error("Interest %d must be between 1 and %d", interest, data_cols);

  SEXP r_ans;
  Rf_protect(r_ans = Rf_allocMatrix(INTSXP, high, outcomes));
  int *ans = INTEGER(r_ans);
  memset(ans, 0, sizeof(int) * high * outcomes);

  int *iresp = INTEGER(VECTOR_ELT(r_data, interest-1));

  for (int rx=0; rx < data_rows; rx++) {
	  bool missing = false;
    int sum=0;
    for (int cx=0; cx < data_cols; cx++) {
      if (!alter && cx+1 == interest) continue;
      int *resp = INTEGER(VECTOR_ELT(r_data, cx));
      if (resp[rx] == NA_INTEGER) {
	      missing = true;
	      break;
      }
      sum += resp[rx] - 1;
    }
    if (missing) continue;
    int pick = iresp[rx];
    if (pick == NA_INTEGER) continue;
    ans[(pick-1) * high + sum] += 1;
  }

  UNPROTECT(1);
  return r_ans;
}

static double table_concordance(double *mat, int rows, int cols, int ii, int jj)
{
  double sum=0;
  for (int hh=ii+1; hh < rows; ++hh) {
    for (int kk=jj+1; kk < cols; ++kk) {
      sum += mat[kk * rows + hh];
    }
  }
  return sum;
}

static double table_discordance(double *mat, int rows, int cols, int ii, int jj)
{
  double sum=0;
  for (int hh=ii+1; hh < rows; ++hh) {
    for (int kk=0; kk < jj; ++kk) {
      sum += mat[kk * rows + hh];
    }
  }
  return sum;
}

/* See Agresti (1990, p. 22) */
SEXP gamma_cor(SEXP r_mat)
{
  int rows;
  int cols;
  getMatrixDims(r_mat, &rows, &cols);
  SEXP realmat;
  Rf_protect(realmat = Rf_coerceVector(r_mat, REALSXP));
  double *mat = REAL(realmat);

  double concord = 0;
  for (int ii=0; ii < rows-1; ++ii) {
    for (int jj=0; jj < cols-1; ++jj) {
      concord += mat[jj * rows + ii] * table_concordance(mat, rows, cols, ii, jj);
    }
  }

  double discord = 0;
  for (int ii=0; ii < rows-1; ++ii) {
    for (int jj=1; jj < cols; ++jj) {
      discord += mat[jj * rows + ii] * table_discordance(mat, rows, cols, ii, jj);
    }
  }

  UNPROTECT(1);

  double gamma = (concord - discord) / (concord + discord);
  return Rf_ScalarReal(gamma);
}