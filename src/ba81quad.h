/*
  Copyright 2012-2014 Joshua Nathaniel Pritikin and contributors

  This is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _BA81QUAD_H_
#define _BA81QUAD_H_

class ba81NormalQuad {
 private:
	void pointToWhere(const int *quad, double *where, int upto);
	void decodeLocation(int qx, const int dims, int *quad);
	double One, ReciprocalOfOne;

	int sIndex(int sx, int qx) {
		//if (sx < 0 || sx >= state->numSpecific) Rf_error("Out of domain");
		//if (qx < 0 || qx >= state->quadGridSize) Rf_error("Out of domain");
		return qx * numSpecific + sx;
	};

	void mapDenseSpace(double piece, const double *where,
			   const double *whereGram, double *latentDist);
	void mapSpecificSpace(int sgroup, double piece, const double *where,
			      const double *whereGram, double *latentDist);

 public:
	int quadGridSize;
	int maxDims;
	int primaryDims;
	int numSpecific;
	int maxAbilities;
	std::vector<double> Qpoint;           // quadGridSize
	int totalQuadPoints;                  // quadGridSize ^ maxDims
	int totalPrimaryPoints;               // totalQuadPoints except for specific dim
	std::vector<double> priQarea;         // totalPrimaryPoints
	std::vector<double> speQarea;         // quadGridSize * numSpecific
	std::vector<double> wherePrep;        // totalQuadPoints * maxDims
	Eigen::MatrixXd whereGram;            // triangleLoc1(maxDims) x totalQuadPoints

	ba81NormalQuad();
	void setOne(double one) { One = one; ReciprocalOfOne = 1/one; }
	void setup0();
	void setup(double Qwidth, int Qpoints, double *means,
		   Eigen::MatrixXd &priCov, Eigen::VectorXd &sVar);
	double getReciprocalOfOne() const { return ReciprocalOfOne; };

	// For dense cov, Dweight is size totalQuadPoints
	// For two-tier, Dweight is numSpecific x totalQuadPoints
	void EAP(double *thrDweight, double scalingFactor, double *scorePad);
};

class ifaGroup {
 private:
	SEXP Rdata;
 public:
	// item description related
	std::vector<const double*> spec;
	int numItems() const { return (int) spec.size(); }
	int paramRows;
	double *param;  // itemParam->data
	std::vector<const char*> itemNames;
	std::vector<int> itemOutcomes;
	std::vector<int> cumItemOutcomes;
	int totalOutcomes;
	std::vector<int> Sgroup;       // item's specific group 0..numSpecific-1

	// latent distribution
	double qwidth;
	int qpoints;
	ba81NormalQuad quad;
	bool twotier;  // rename to detectTwoTier TODO
	int maxAbilities;
	int numSpecific;
	double *mean;
	double *cov;
	std::vector<const char*> factorNames;

	// data related
	std::vector<const int*> dataColumns;
	std::vector<int> rowMap;       // row index into MxData
	int dataRows;
	const char *weightColumnName;
	double *rowWeight;

	// workspace
	double *outcomeProb;                  // totalOutcomes * totalQuadPoints

	// TODO:
	// scores

	ifaGroup(bool _twotier);
	~ifaGroup();
	void setGridFineness(double width, int points);
	void import(SEXP Rlist);
	void importSpec(SEXP slotValue);
	void setLatentDistribution(int dims, double *mean, double *cov);
	double *getItemParam(int ix) { return param + paramRows * ix; }
	double area(int qx, int ix);
	const int *dataColumn(int col) { return dataColumns[col]; };
	void detectTwoTier();
	void sanityCheck();
	void ba81OutcomeProb(double *param, bool wantLog);
	void ba81LikelihoodSlow2(const int px, double *out);
	void cai2010EiEis(const int px, double *lxk, double *Eis, double *Ei);
};

#endif
