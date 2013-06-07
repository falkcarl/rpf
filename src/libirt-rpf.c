/*
  Copyright 2012-2013 Joshua Nathaniel Pritikin and contributors

  libirt-rpf is free software: you can redistribute it and/or modify
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

#include <R.h>
#include <R_ext/BLAS.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "../inst/include/libirt-rpf.h"

#ifndef M_LN2
#define M_LN2           0.693147180559945309417232121458        /* ln(2) */
#endif

#ifndef M_LN_SQRT_PI
#define M_LN_SQRT_PI    0.572364942924700087071713675677        /* log(sqrt(pi))
                                                                   == log(pi)/2 */
#endif

static const double EXP_STABLE_DOMAIN = 35;

// This is far away from the item's difficulty so it is less
// interesting for estimation and the gradient becomes numerically
// unstable as athb < -25. For symmetry, it is necessary to clamp at
// 25 as well.
static const double GRADIENT_STABLE_DOMAIN = 25;

static int
model_name_to_id(const char *target)
{
  for (int sx=0; sx < librpf_numModels; sx++) {
    if (strcmp(librpf_model[sx].name, target) == 0) return sx;
  }
  return -1;
}

static void
irt_rpf_logprob_adapter(const double *spec,
			const double *restrict param, const double *restrict th,
			double *restrict out)
{
  (*librpf_model[(int) spec[RPF_ISpecID]].prob)(spec, param, th, out);

  int numOutcomes = spec[RPF_ISpecOutcomes];
  for (int ox=0; ox < numOutcomes; ox++) {
    out[ox] = log(out[ox]);
  }
}

static double
dotprod(const double *v1, const double *v2, const int len)
{
  double dprod = 0;
  for (int dx=0; dx < len; dx++) {
    dprod += v1[dx] * v2[dx];
  }
  return dprod;
}

static int
hessianIndex(int numParam, int row, int col)
{
  return numParam + row*(row+1)/2 + col;
}

/**
 * lognormal(x,sd) := (x*sd*sqrt(2*%pi))^-1 * exp(-(log(x)^2)/(2*sd^2))
 * log(lognormal(x,sd)), logexpand=super;
 */
static double lognormal_pdf(double aa, double sd)
{
  double sd2 = sd*sd;
  double loga = log(aa);
  return -loga*loga/(2*sd2) - loga - log(sd) - M_LN_SQRT_PI - M_LN2/2;
}

/**
 * diff(log(lognormal(a,sd))),a), logexpand=super;
 */
static double lognormal_gradient(double aa, double sd)
{
  double sd2 = sd*sd;
  return -log(aa)/(aa * sd2) - 1/aa;
}

static double logit(double prob)
{
  return log(prob/(1-prob));
}

// Prior for the lower asymptote parameter (Cai, Yang & Hansen, 2011, p. 246)

/**
 * normal(x,mean,sd) := (sd*sqrt(2*%pi))^-1 * exp(-((x - mean)^2)/(2*sd^2))
 * log(normal(log(x/(1-x)),mean,sd)), logexpand=super;
 */
static double logitnormal_pdf(double cc, double mean, double sd)
{
  double sd2 = sd * sd;
  double offset = logit(cc) - mean;
  return -(offset*offset)/(2*sd2) - log(sd) - M_LN_SQRT_PI - M_LN2/2;
}

/**
 * factor(diff(log(normal(log(x/(1-x)),mean,sd)),x))
 */
static double logitnormal_gradient(double cc, double mean, double sd)
{
  double sd2 = sd * sd;
  return (logit(cc) - mean) / (sd2 * (cc-1) * cc);
}

static int
irt_rpf_1dim_drm_numSpec(const double *spec)
{ return RPF_ISpecCount + 3; }

static int
irt_rpf_1dim_drm_numParam(const double *spec)
{ return 4; }

static void
irt_rpf_1dim_drm_prob(const double *spec,
		      const double *restrict param, const double *restrict th,
		      double *restrict out)
{
  double guessing = param[2];
  double upper = param[3];
  double athb = -param[0] * (th[0] - param[1]);
  if (athb < -EXP_STABLE_DOMAIN) athb = -EXP_STABLE_DOMAIN;
  else if (athb > EXP_STABLE_DOMAIN) athb = EXP_STABLE_DOMAIN;
  double pp = guessing + (upper-guessing) / (1 + exp(athb));
  out[0] = 1-pp;
  out[1] = pp;
}

static double
irt_rpf_1dim_drm_prior(const double *spec,
		       const double *restrict param)
{
  const double *prior = spec + RPF_ISpecCount;
  double ll = 0; //lognormal_pdf(param[0], prior[0]);
  double cc = param[2];
  if (cc > 0) {
    ll += logitnormal_pdf(cc, prior[1], prior[2]);
  }
  return ll;
}

static void
set_deriv_nan(const double *spec, double *out)
{
  int numParam = (*librpf_model[(int) spec[RPF_ISpecID]].numParam)(spec);

  for (int px=0; px < numParam; px++) {
    out[px] = nan("I");
  }
}

/**
 * irf(a,b,c,th) := c+(1-c)/(1+exp(-a*(th - b)))
 * diff(log(1-irf(a,b,c,th)),a);  // 0
 * diff(log(irf(a,b,c,th)),a);    // 1
 * diff(log(1-irf(a,b,c,th)),b);  // 0
 * diff(log(irf(a,b,c,th)),b);    // 1
 * ratsimp(diff(log(1-irf(a,b,c,th)),c));
 * diff(log(irf(a,b,c,th)),c);
 */
static void
irt_rpf_1dim_drm_deriv1(const double *spec,
			const double *restrict param,
			const double *where, const double area,
			const double *weight, double *out)
{
  double aa = param[0];
  double bb = param[1];
  double cc = param[2];
  double th = where[0];
  double athb = -aa * (th - bb);

  if (athb < -GRADIENT_STABLE_DOMAIN) athb = -GRADIENT_STABLE_DOMAIN;
  else if (athb > GRADIENT_STABLE_DOMAIN) athb = GRADIENT_STABLE_DOMAIN;

  double Eathb = exp(athb);
  double Eathb2 = (Eathb+1)*(Eathb+1);
  double w0 = Eathb2 * (-(1-cc)/(Eathb+1) - cc + 1);
  double w1 = Eathb2 * ((1-cc)/(Eathb+1) + cc);

  out[0] += area * ((weight[0] * (bb-th)*Eathb / w0 +
		     weight[1] * -(bb-th)*Eathb / w1));
  out[1] += area * ((weight[0] * Eathb / w0 +
		     weight[1] * -Eathb / w1));
  double ratio = (1-(1/(Eathb+1))) / ((1-cc)/(Eathb+1) + cc);
  out[2] += area * ((weight[0] * (1/(cc-1)) +
		     weight[1] * ratio));
}

static void
irt_rpf_1dim_drm_deriv2(const double *spec, const double *restrict param,
			double *out)
{
  double aa = param[0];
  double cc = param[2];
  const double *prior = spec + RPF_ISpecCount;
  if (aa <= 0 || cc < 0 || cc >= 1) {
    set_deriv_nan(spec, out);
    return;
  }

  out[0] *= (1-cc);
  //out[0] += lognormal_gradient(aa, prior[0]);

  out[1] *= aa * (1-cc);
  if (cc == 0) { out[2] = nan("I"); }
  else {
    out[2] += logitnormal_gradient(cc, prior[1], prior[2]);
  }
  for (int px=3; px < 9; px++) out[px] = nan("I");
}

static void
irt_rpf_1dim_drm_rescale(const double *spec, double *restrict param, const int *paramMask,
			 const double *restrict mean, const double *restrict choleskyCov)
{
  double thresh = param[1] * -param[0];
  if (paramMask[0] >= 0) {
    param[0] *= choleskyCov[0];
  }
  if (paramMask[1] >= 0) {
    thresh += param[0] * mean[0];
    param[1] = thresh / -param[0];
  }
}

static void
irt_rpf_1dim_drm_prefit(double *spec, double *restrict param)
{
  param[1] *= -param[0];
  spec[RPF_ISpecID] = model_name_to_id("drm1+");
}

static void
irt_rpf_1dim_drm_postfit(double *spec, double *restrict param)
{
  param[1] /= -param[0];
  spec[RPF_ISpecID] = model_name_to_id("drm1");
}

static int
irt_rpf_mdim_drm_numSpec(const double *spec)
{ return RPF_ISpecCount + 3; }

static int
irt_rpf_mdim_drm_numParam(const double *spec)
{ return 3 + spec[RPF_ISpecDims]; }

static void
irt_rpf_mdim_drm_prob(const double *spec,
		      const double *restrict param, const double *restrict th,
		      double *restrict out)
{
  int numDims = spec[RPF_ISpecDims];
  double dprod = dotprod(param, th, numDims);
  double diff = param[numDims];
  double guessing = param[numDims+1];
  double upper = param[numDims+2];
  double athb = -(dprod + diff);
  if (athb < -EXP_STABLE_DOMAIN) athb = -EXP_STABLE_DOMAIN;
  else if (athb > EXP_STABLE_DOMAIN) athb = EXP_STABLE_DOMAIN;
  double tmp = guessing + (upper-guessing) / (1 + exp(athb));
  out[0] = 1-tmp;
  out[1] = tmp;
}

static void
irt_rpf_mdim_drm_prob2(const double *spec,
		       const double *restrict param, const double *restrict th,
		       double *restrict out1, double *restrict out2)
{
  int numDims = spec[RPF_ISpecDims];
  double dprod = dotprod(param, th, numDims);
  double diff = param[numDims];
  double guessing = param[numDims+1];
  double upper = param[numDims+2];
  double athb = -(dprod + diff);
  if (athb < -EXP_STABLE_DOMAIN) athb = -EXP_STABLE_DOMAIN;
  else if (athb > EXP_STABLE_DOMAIN) athb = EXP_STABLE_DOMAIN;
  double tmp = 1 / (1 + exp(athb));
  out1[0] = 1-tmp;
  out1[1] = tmp;
  tmp = guessing + (upper-guessing) * tmp;
  out2[0] = 1-tmp;
  out2[1] = tmp;
}

static double
irt_rpf_mdim_drm_prior(const double *spec,
		       const double *restrict param)
{
  int numDims = spec[RPF_ISpecDims];
  const double *prior = spec + RPF_ISpecCount;
  double ll=0;
  for (int dx=0; dx < numDims; dx++) {
    //if (param[dx] > 0) ll += lognormal_pdf(param[dx], prior[0]);
  }
  double cc = param[numDims+1];
  if (cc > 0) {
    ll += logitnormal_pdf(cc, prior[1], prior[2]);
  }
  return ll;
}

/**
 * irf(a1,a2,b,c,th1,th2) := c+(1-c)/(1+exp(-(a1*th1+a2*th2+b)));
 * ratsimp(diff(log(1-irf(a1,a2,b,c,th1,th2)),a1)); //0
 * diff(log(1-irf(a1,a2,b,c,th1,th2)),a2);
 * diff(log(1-irf(a1,a2,b,c,th1,th2)),b);
 * diff(log(1-irf(a1,a2,b,c,th1,th2)),c);
 *
 * Wolfram Alpha
 * d/dx log(1- (c+(1-c)/(1+exp(-(x*f+y*g+z*h+b)))))
 * d/db log( (c+(1-c)/(1+exp(-(x*f+y*g+z*h+b)))))
 */
static void
irt_rpf_mdim_drm_deriv1(const double *spec,
		       const double *restrict param,
		       const double *where, const double area,
		       const double *weight, double *out)
{
  int numDims = spec[RPF_ISpecDims];
  const double *aa = param;
  double bb = param[numDims];
  double cc = param[numDims+1];

  double athb = dotprod(aa, where, numDims) + bb;
  if (athb < -GRADIENT_STABLE_DOMAIN) athb = -GRADIENT_STABLE_DOMAIN;
  else if (athb > GRADIENT_STABLE_DOMAIN) athb = GRADIENT_STABLE_DOMAIN;

  double Eathb = exp(athb);
  for (int dx=0; dx < numDims; dx++) {
    out[dx] += area * ((weight[0] * (where[dx]/(Eathb+1) - where[dx]) +
			weight[1] * (where[dx]/(Eathb+1) - cc * where[dx] / (Eathb+cc))));
  }
  out[numDims] += area * ((weight[0] * (1/(Eathb+1) - 1) +
			   weight[1] * (-((cc-1)*Eathb)/((Eathb+1)*(Eathb+cc)))));
  out[numDims+1] += area * ((weight[0] * (1/(cc-1)) +
			     weight[1] * (1/(Eathb+cc))));
}

static void
irt_rpf_mdim_drm_deriv2(const double *spec,
			const double *restrict param,
			double *out)
{
  int numDims = spec[RPF_ISpecDims];
  const double *aa = param;
  double cc = param[numDims+1];
  if (cc < 0 || cc >= 1) {
    set_deriv_nan(spec, out);
    return;
  }
  for (int dx=0; dx < numDims; dx++) {
    if (aa[dx] < 0) {
      set_deriv_nan(spec, out);
      return;
    }
  }
  const double *prior = spec + RPF_ISpecCount;
  for (int dx=0; dx < numDims; dx++) {
    //out[dx] += lognormal_gradient(aa[dx], prior[0]);
  }
  if (cc == 0) {
    out[numDims+1] = nan("I");
  } else {
    out[numDims+1] += logitnormal_gradient(cc, prior[1], prior[2]);
  }
  const int numH = (numDims+2) * (numDims+3) / 2;
  for (int px=numDims+2; px < numDims+2 + numH; px++) out[px] = nan("I");
}

static void
irt_rpf_mdim_drm_deriv_phil1(const double *spec,             // remove old impl TODO
		       const double *restrict param,
		       const double *where, const double area,
		       const double *weight, double *out)
{
  int numDims = spec[RPF_ISpecDims];
  double cc = param[numDims+1];
  double upper = param[numDims+2];
  double PQ[2];
  double PQstar[2];
  irt_rpf_mdim_drm_prob2(spec, param, where, PQstar, PQ);
  double r1 = weight[0];
  double r2 = weight[1];
  double r1_P = r1/PQ[0];
  double r1_P2 = r1/(PQ[0] * PQ[0]);
  double r2_Q = r2/PQ[1];
  double r2_Q2 = r2/(PQ[1] * PQ[1]);
  for (int dx=0; dx < numDims; dx++) {
    out[dx] -= area * where[dx] * PQstar[0] * PQstar[1] * (upper-cc) * (r1_P - r2_Q);
  }
  out[numDims] -= area * (upper-cc) * PQstar[0] * PQstar[1] * (r1_P - r2_Q);
  out[numDims+1] -= area * PQstar[0] * (r1_P - r2_Q);
  out[numDims+2] -= area * PQstar[1] * (r1_P - r2_Q);

  int ox = numDims+2;
  double ugD2 = (upper-cc);  // can factor out TODO
  double ugD = (upper-cc);
  double Pstar = PQstar[0];
  double Pstar2 = Pstar * Pstar;
  double Pstar3 = Pstar2 * Pstar;
  double Qstar = PQstar[1];
  double Qstar2 = Qstar * Qstar;

  for(int ix=0; ix < numDims; ix++) {
    for(int jx=0; jx <= ix; jx++) {
      out[++ox] += area * (r1_P * (ugD2 * where[ix] * where[jx] *
				   (Pstar - 3*Pstar2 + 2*Pstar3)) -
			   r1_P2 * (ugD * where[ix] * (Pstar - Pstar2) *
				    (ugD * where[jx] * (Pstar - Pstar2))) +
			   r2_Q * (ugD2 * where[ix] * where[jx] *
				   (-Pstar + 3*Pstar2 - 2*Pstar3)) -
			   r2_Q2 * (ugD * where[ix] * (-Pstar + Pstar2) *
				    (ugD * where[jx] * (-Pstar + Pstar2))));
    }
  }
  for(int ix=0; ix < numDims; ix++) {
    out[++ox] += area * (r1_P * (ugD2 * where[ix] * (Pstar - 3*Pstar2 + 2*Pstar3)) -
			 r1_P2 * (ugD * where[ix] * (Pstar - Pstar2) *
				  (ugD * (Pstar - Pstar2))) +
			 r2_Q * (ugD2 * where[ix] * (-Pstar + 3*Pstar2 - 2*Pstar3)) -
			 r2_Q2 * (ugD * where[ix] * (-Pstar + Pstar2) *
				  (ugD * (-Pstar + Pstar2))));
  }
  double chunk1 = ugD * (Pstar - Pstar2);
  out[++ox] += area * (r1_P * (ugD2 * (Pstar - 3*Pstar2 + 2*Pstar3)) -
		       r1_P2 * chunk1*chunk1 +
		       r2_Q * (ugD2 * (-Pstar + 3*Pstar2 - 2*Pstar3)) -
		       r2_Q2 * chunk1*chunk1);
  for(int ix=0; ix < numDims; ix++) {
    out[++ox] += area * (r1_P * (where[ix] * (Pstar - Pstar2)) -
			 r1_P2 * (ugD * where[ix] * (Pstar - Pstar2)) * Pstar +
			 r2_Q * (where[ix] * (-Pstar + Pstar2)) +
			 r2_Q2 * (ugD * where[ix] * (-Pstar + Pstar2) ) * Pstar);
  }
  out[++ox] += area * (r1_P * (Pstar - Pstar2) -
		       r1_P2 * (ugD * (Pstar - Pstar2)) * Pstar +
		       r2_Q * (-Pstar + Pstar2) +
		       r2_Q2 * (ugD * (-Pstar + Pstar2)) * Pstar);
  out[++ox] -= area * Pstar2 * (r1_P2 + r2_Q2);

  for(int ix=0; ix < numDims; ix++) {
    out[++ox] += area * (r1_P * (where[ix] * (-Pstar + Pstar2)) -
			 r1_P2 * (ugD * where[ix] * (Pstar - Pstar2)) * Qstar +
			 r2_Q * (where[ix] * (Pstar - Pstar2)) -
			 r2_Q2 * (ugD * where[ix] * (-Pstar + Pstar2) ) * (-1 + Pstar));
  }

  out[++ox] += area * (r1_P * (-Pstar + Pstar2) -
		       r1_P2 * (ugD * (Pstar - Pstar2)) * Qstar +
		       r2_Q * (Pstar - Pstar2) -
		       r2_Q2 * (ugD * (-Pstar + Pstar2)) * -Qstar);

  out[++ox] += area * (-r1_P2 * Pstar * Qstar + r2_Q2 * Pstar * (-1 + Pstar));
  out[++ox] -= area * Qstar2 * (r1_P2 + r2_Q2);
}

static void
irt_rpf_mdim_drm_deriv_phil2(const double *spec,
			     const double *restrict param,
			     double *out)
{
  int numDims = spec[RPF_ISpecDims];
  const double *aa = param;
  double cc = param[numDims+1];
  double upper = param[numDims+2];

  if (cc < 0 || cc >= 1) {
    set_deriv_nan(spec, out);
    return;
  }
  for (int dx=0; dx < numDims; dx++) {
    if (aa[dx] < 0) {
      set_deriv_nan(spec, out);
      return;
    }
  }
  if (cc == 0) {
    out[numDims+1] = nan("I");
  }
  if (upper == 1) {
    out[numDims+2] = nan("I");
  }
}

static void
irt_rpf_mdim_drm_rescale(const double *spec, double *restrict param, const int *paramMask,
			 const double *restrict mean, const double *restrict choleskyCov)
{
  int numDims = spec[RPF_ISpecDims];
  for (int d1=0; d1 < numDims; d1++) {
    if (paramMask[d1] < 0) continue;
    double result=0;
    for (int d2=d1; d2 < numDims; d2++) {
      result += param[d2] * choleskyCov[d1 * numDims + d2];
    }
    param[d1] = result;
    param[numDims] += result * mean[d1];
  }
}

static void
irt_rpf_1dim_grm_prob(const double *spec,
		      const double *restrict param, const double *restrict th,
		      double *restrict out)
{
  const int numOutcomes = spec[RPF_ISpecOutcomes];
  const double slope = param[0];
  const double theta = th[0];
  const double *kat = param + (int) spec[RPF_ISpecDims];

  double athb = -slope * (theta - kat[0]);
  if (athb < -EXP_STABLE_DOMAIN) athb = -EXP_STABLE_DOMAIN;
  else if (athb > EXP_STABLE_DOMAIN) athb = EXP_STABLE_DOMAIN;
  double tmp = 1 / (1 + exp(athb));
  out[0] = 1-tmp;
  out[1] = tmp;

  for (int kx=2; kx < numOutcomes; kx++) {
    double athb = -slope * (theta - kat[kx-1]);
    if (athb < -EXP_STABLE_DOMAIN) athb = -EXP_STABLE_DOMAIN;
    else if (athb > EXP_STABLE_DOMAIN) athb = EXP_STABLE_DOMAIN;
    double tmp = 1 / (1 + exp(athb));
    out[kx-1] -= tmp;
    out[kx] = tmp;
  }
}

static int
irt_rpf_mdim_grm_numSpec(const double *spec)
{ return RPF_ISpecCount; }

static int
irt_rpf_mdim_grm_numParam(const double *spec)
{ return spec[RPF_ISpecOutcomes] + spec[RPF_ISpecDims] - 1; }

static void
irt_rpf_mdim_grm_prob(const double *spec,
		      const double *restrict param, const double *restrict th,
		      double *restrict out)
{
  const int numDims = spec[RPF_ISpecDims];
  const int numOutcomes = spec[RPF_ISpecOutcomes];
  const double *slope = param;
  const double dprod = dotprod(slope, th, numDims);
  const double *kat = param + (int) spec[RPF_ISpecDims];

  double athb = -(dprod + kat[0]);
  if (athb < -EXP_STABLE_DOMAIN) athb = -EXP_STABLE_DOMAIN;
  else if (athb > EXP_STABLE_DOMAIN) athb = EXP_STABLE_DOMAIN;
  double tmp = 1 / (1 + exp(athb));
  out[0] = 1-tmp;
  out[1] = tmp;

  for (int kx=2; kx < numOutcomes; kx++) {
    double athb = -(dprod + kat[kx-1]);
    if (athb < -EXP_STABLE_DOMAIN) athb = -EXP_STABLE_DOMAIN;
    else if (athb > EXP_STABLE_DOMAIN) athb = EXP_STABLE_DOMAIN;
    double tmp = 1 / (1 + exp(athb));
    out[kx-1] -= tmp;
    out[kx] = tmp;
  }
}

static void
irt_rpf_mdim_grm_rawprob(const double *spec,
			 const double *restrict param, const double *restrict th,
			 double *restrict out)
{
  int numDims = spec[RPF_ISpecDims];
  const int numOutcomes = spec[RPF_ISpecOutcomes];
  const double dprod = dotprod(param, th, numDims);
  const double *kat = param + (int) spec[RPF_ISpecDims];

  out[0] = 1;
  for (int kx=0; kx < numOutcomes-1; kx++) {
    double athb = -(dprod + kat[kx]);
    if (athb < -EXP_STABLE_DOMAIN) athb = -EXP_STABLE_DOMAIN;
    else if (athb > EXP_STABLE_DOMAIN) athb = EXP_STABLE_DOMAIN;
    double tmp = 1 / (1 + exp(athb));
    out[kx+1] = tmp;
  }
  out[numOutcomes] = 0;
}

static void
irt_rpf_mdim_grm_deriv1(const double *spec,
			const double *restrict param,
			const double *where, const double area,
			const double *weight, double *out)
{
  int nfact = spec[RPF_ISpecDims];
  int outcomes = spec[RPF_ISpecOutcomes];
  int nzeta = spec[RPF_ISpecOutcomes] - 1;
  double P[nzeta+2];
  double PQfull[nzeta+2];
  irt_rpf_mdim_grm_rawprob(spec, param, where, P);
  PQfull[0] = 0;
  PQfull[outcomes] = 0;
  for (int kx=1; kx <= nzeta; kx++) PQfull[kx] = P[kx] * (1-P[kx]);
  for (int jx = 0; jx <= nzeta; jx++) {
    double Pk_1 = P[jx];
    double Pk = P[jx + 1];
    double PQ_1 = PQfull[jx];
    double PQ = PQfull[jx + 1];
    double Pk_1Pk = Pk_1 - Pk;
    if (Pk_1Pk < 1e-10) Pk_1Pk = 1e-10;
    double dif1 = weight[jx] / Pk_1Pk;
    double dif1sq = weight[jx] / (Pk_1Pk * Pk_1Pk);
    if(jx < nzeta) {
      double Pk_p1 = P[jx + 2];
      double PQ_p1 = PQfull[jx + 2];
      double Pk_Pkp1 = Pk - Pk_p1;
      if(Pk_Pkp1 < 1e-10) Pk_Pkp1 = 1e-10;
      double dif2 = weight[jx+1] / Pk_Pkp1;
      double dif2sq = weight[jx+1] / (Pk_Pkp1 * Pk_Pkp1);
      out[nfact + jx] -= area * PQ * (dif1 - dif2);

      int d2base = (nfact + nzeta) + (nfact+jx) * (nfact+jx+1)/2;
      out[d2base + nfact + jx] += area * (-1 * PQ * PQ * (dif1sq + dif2sq) -
					  (dif1 - dif2) * (Pk * (1.0 - Pk) * (1.0 - 2.0*Pk)));
      if (jx < (nzeta - 1)) {
	int d2base1 = (nfact + nzeta) + (nfact+jx+1) * (nfact+jx+2)/2;
	out[d2base1 + nfact + jx] += area * dif2sq * PQ_p1 * PQ;
      }
      double tmp1 = (-1.0) * dif2sq * PQ * (PQ - PQ_p1);
      double tmp2 = dif1sq * PQ * (PQ_1 - PQ);
      double tmp3 = (dif1 - dif2) * (Pk * (1.0 - Pk) * (1.0 - 2.0*Pk));
      for(int kx = 0; kx < nfact; kx++){
	out[d2base + kx] += area * (tmp1 + tmp2 - tmp3) * where[kx];
      }
    }
    for(int kx = 0; kx < nfact; kx++) {
      out[kx] += area * dif1 * (PQ_1 - PQ) * where[kx];
    }

    double temp[nfact];
    for(int ix = 0; ix < nfact; ix++)
      temp[ix] = PQ_1 * where[ix] - PQ * where[ix];

    int d2x = nfact + nzeta;
    for(int i = 0; i < nfact; i++) {
      for(int j = 0; j <= i; j++) {
	double outer = where[i]*where[j];
	out[d2x++] += area * (-1 * dif1sq * temp[i] * temp[j] +
			      (dif1 * (Pk_1 * (1.0 - Pk_1) * (1.0 - 2.0 * Pk_1) * 
				       outer - Pk * (1.0 - Pk) * (1.0 - 2.0 * Pk) * outer)));
      }
    }
  }
}

static void
irt_rpf_mdim_grm_deriv2(const double *spec,
			const double *restrict param,
			double *out)
{
  int nfact = spec[RPF_ISpecDims];
  int nzeta = spec[RPF_ISpecOutcomes] - 1;
  const double *aa = param;
  for (int dx=0; dx < nfact; dx++) {
    if (aa[dx] < 0) {
      set_deriv_nan(spec, out);
      return;
    }
  }
  for (int zx=0; zx < nzeta-1; zx++) {
    if (param[nfact+zx] < param[nfact+zx+1]) {
      set_deriv_nan(spec, out);
      return;
    }
  }
}

static int
irt_rpf_nominal_numSpec(const double *spec)
{
  int outcomes = spec[RPF_ISpecOutcomes];
  int Tlen = (outcomes - 1) * (outcomes - 1);
  return RPF_ISpecCount + 2 * Tlen;
}

static int
irt_rpf_nominal_numParam(const double *spec)
{ return spec[RPF_ISpecDims] + 2 * (spec[RPF_ISpecOutcomes]-1); }

static void
irt_rpf_nominal_prob(const double *spec,
		     const double *restrict param, const double *restrict th,
		     double *restrict out)
{
  int numDims = spec[RPF_ISpecDims];
  int numOutcomes = spec[RPF_ISpecOutcomes];
  double discr = dotprod(param, th, numDims);
  const double *alpha = param + numDims;
  const double *gamma = param + numDims + numOutcomes - 1;
  const double *Ta = spec + RPF_ISpecCount;
  const double *Tc = spec + RPF_ISpecCount + (numOutcomes-1) * (numOutcomes-1);

  double num[numOutcomes];
  double den = 0;
  for (int kx=0; kx < numOutcomes; kx++) {
    double ak = 0;
    double ck = 0;
    if (kx) {
      for (int tx=0; tx < numOutcomes-1; tx++) {
	int Tcell = tx * (numOutcomes-1) + kx-1;
	ak += Ta[Tcell] * alpha[tx];
	ck += Tc[Tcell] * gamma[tx];
      }
    }

    double z = discr * ak + ck;
    if (z < -EXP_STABLE_DOMAIN) z = -EXP_STABLE_DOMAIN;
    else if (z > EXP_STABLE_DOMAIN) z = EXP_STABLE_DOMAIN;
    num[kx] = exp(z);
    den += num[kx];
  }

  for (int kx=0; kx < numOutcomes; kx++) {
    out[kx] = num[kx]/den;
  }
}

static void
irt_rpf_nominal_logprob(const double *spec,
			const double *restrict param, const double *restrict th,
			double *restrict out)
{
  int numDims = spec[RPF_ISpecDims];
  int numOutcomes = spec[RPF_ISpecOutcomes];
  double discr = dotprod(param, th, numDims);
  const double *alpha = param + numDims;
  const double *gamma = param + numDims + numOutcomes - 1;
  const double *Ta = spec + RPF_ISpecCount;
  const double *Tc = spec + RPF_ISpecCount + (numOutcomes-1) * (numOutcomes-1);

  double num[numOutcomes];
  double den = 0;
  for (int kx=0; kx < numOutcomes; kx++) {
    double ak = 0;
    double ck = 0;
    if (kx) {
      for (int tx=0; tx < numOutcomes-1; tx++) {
	int Tcell = tx * (numOutcomes-1) + kx-1;
	ak += Ta[Tcell] * alpha[tx];
	ck += Tc[Tcell] * gamma[tx];
      }
    }

    double z = discr * ak + ck;
    if (z < -EXP_STABLE_DOMAIN) z = -EXP_STABLE_DOMAIN;
    else if (z > EXP_STABLE_DOMAIN) z = EXP_STABLE_DOMAIN;
    num[kx] = z;
    den += exp(z);
  }

  den = log(den);
  for (int kx=0; kx < numOutcomes; kx++) {
    out[kx] = num[kx] - den;
  }
}

static double makeOffterm(const double *dat, const double p, const double aTheta,
			  const int ncat, const int cat)
{
  double ret = 0;
  for (int CAT = 0; CAT < ncat; CAT++) {
    if (CAT == cat) continue;
    ret += dat[CAT] * p * aTheta;
  }
  return(ret);
}

static double makeOffterm2(const double *dat, const double p1, const double p2, 
			   const double aTheta, const int ncat, const int cat)
{
  double ret = 0;
  for (int CAT = 0; CAT < ncat; CAT++) {
    if (CAT == cat) continue;
    ret += dat[CAT] * p1 * p2 * aTheta;
  }
  return(ret);
}

static void
irt_rpf_nominal_deriv1(const double *spec,
		       const double *restrict param,
		       const double *where, const double area,
		       const double *weight, double *out)
{
  int nfact = spec[RPF_ISpecDims];
  int ncat = spec[RPF_ISpecOutcomes];
  double aTheta = dotprod(param, where, nfact);
  double aTheta2 = aTheta * aTheta;

  double num[ncat];
  double P[ncat];
  double P2[ncat];
  double P3[ncat];
  double ak[ncat];
  double ak2[ncat];
  double dat_num[ncat];
  double numsum = 0;
  double numakD = 0;
  double numak2D2 = 0;
  double numakDTheta_numsum[nfact];

  const double *alpha = param + nfact;
  const double *gamma = param + nfact + ncat - 1;
  const double *Ta = spec + RPF_ISpecCount;
  const double *Tc = spec + RPF_ISpecCount + (ncat-1) * (ncat-1);

  for (int kx=0; kx < ncat; kx++) {
    double ak1 = 0;
    double ck1 = 0;
    if (kx) {
      for (int tx=0; tx < ncat-1; tx++) {
	int Tcell = tx * (ncat-1) + kx - 1;
	ak1 += Ta[Tcell] * alpha[tx];
	ck1 += Tc[Tcell] * gamma[tx];
      }
    }

    double z = aTheta * ak1 + ck1;
    if (z < -EXP_STABLE_DOMAIN) z = -EXP_STABLE_DOMAIN;
    else if (z > EXP_STABLE_DOMAIN) z = EXP_STABLE_DOMAIN;
    num[kx] = exp(z);
    ak[kx] = ak1;
    ak2[kx] = ak1 * ak1;
    dat_num[kx] = weight[kx]/num[kx];
    numsum += num[kx];
    numakD += num[kx] * ak1;
    numak2D2 += num[kx] * ak2[kx];
  }
  double numsum2 = numsum * numsum;

  for (int kx=0; kx < ncat; kx++) {
    P[kx] = num[kx]/numsum;
    P2[kx] = P[kx] * P[kx];
    P3[kx] = P2[kx] * P[kx];
  }

  double sumNumak = dotprod(num, ak, ncat);
  for (int fx=0; fx < nfact; fx++) {
    numakDTheta_numsum[fx] = sumNumak * where[fx] / numsum;
  }

  for (int jx = 0; jx < nfact; jx++) {
    double tmpvec = 0;
    for(int i = 0; i < ncat; i++) {
      tmpvec += dat_num[i] * (ak[i] * where[jx] * P[i] -
			      P[i] * numakDTheta_numsum[jx]) * numsum;
    }
    out[jx] += area * tmpvec;
  }
  for(int i = 1; i < ncat; i++) {
    double offterm = makeOffterm(weight, P[i], aTheta, ncat, i);
    double offterm2 = makeOffterm(weight, P[i], 1, ncat, i);
    double tmpvec = dat_num[i] * (aTheta * P[i] - P2[i] * aTheta) * numsum - offterm;
    double tmpvec2 = dat_num[i] * (P[i] - P2[i]) * numsum - offterm2;
    out[nfact + i - 1] += area * tmpvec;
    out[nfact + ncat + i - 2] += area * tmpvec2;
  }

  int hessbase = nfact + (ncat-1)*2;
  int d2ind = 0;
  //a's
  for (int j = 0; j < nfact; j++) {
    for (int k = 0; k <= j; k++) {
      double tmpvec = 0;
      for (int i = 0; i < ncat; i++) {
	tmpvec += dat_num[i] * (ak2[i] * where[j] * where[k] * P[i] -
				ak[i] * where[j] * P[i] * numakDTheta_numsum[k] -
				ak[i] * where[k] * P[i] * numakDTheta_numsum[j] + 
				2 * P[i] * numakD * where[j] * numakD * where[k] / numsum2 -
				P[i] * numak2D2 * where[j] * where[k] / numsum) * numsum - 
	  dat_num[i] * (ak[i] * where[j] * P[i] - P[i] * numakDTheta_numsum[j]) *
	  numsum * ak[i] * where[k] +
	  dat_num[i] * (ak[i] * where[j] * P[i] - P[i] * numakDTheta_numsum[j]) *
	  numakD * where[k];
      }
      out[hessbase + d2ind++] += area * tmpvec;
    }
  }
  //a's with ak and d
  for(int k = 1; k < ncat; k++){
    int akrow = hessbase + (nfact+k)*(nfact+k-1)/2;
    int dkrow = hessbase + (nfact+ncat+k-1)*(nfact+ncat+k-2)/2;
    for(int j = 0; j < nfact; j++){
      double tmpvec = 0;
      double tmpvec2 = 0;
      for(int i = 0; i < ncat; i++){
	if(i == k){
	  tmpvec += dat_num[i] * (ak[i]*where[j] * aTheta*P[i] -
				     aTheta*P[i]*numakDTheta_numsum[j] +
				     where[j]*P[i] - 2*ak[i]*where[j]*aTheta*P2[i] +
				     2*aTheta*P2[i]*numakDTheta_numsum[j] -
				     where[j]*P2[i])*numsum -
	    dat_num[i]*(aTheta*P[i] - aTheta*P2[i])*numsum*ak[i]*where[j] +
	    dat_num[i]*(aTheta*P[i] - aTheta*P2[i])*(numakD*where[j]);
	  tmpvec2 += dat_num[i]*(ak[i]*where[j]*P[i] -
				      2*ak[i]*where[j]*P2[i] -
				      P[i]*numakDTheta_numsum[j] +
				      2*P2[i]*numakDTheta_numsum[j])*numsum -
	    dat_num[i]*(P[i] - P2[i])*numsum*ak[i]*where[j] +
	    dat_num[i]*(P[i] - P2[i])*(numakD*where[j]);
	} else {
	  tmpvec += -weight[i]*ak[k]*aTheta*where[j]*P[k] +
	    weight[i]*P[k]*aTheta*numakDTheta_numsum[j] -
	    weight[i]*P[k]*where[j];
	  tmpvec2 += -weight[i]*ak[k]*where[j]*P[k] +
	    weight[i]*P[k]*numakDTheta_numsum[j];
	}
      }
      out[akrow + j] += area * tmpvec;
      out[dkrow + j] += area * tmpvec2;
    }
  }
  //ak's and d's
  for(int j = 1; j < ncat; j++){
    int akrow = hessbase + (nfact+j)*(nfact+j-1)/2;
    int dkrow = hessbase + (nfact+ncat+j-1)*(nfact+ncat+j-2)/2;

    double tmpvec = makeOffterm(weight, P2[j], aTheta2, ncat, j);
    double tmpvec2 = makeOffterm(weight, P[j], aTheta2, ncat, j);
    double offterm = tmpvec - tmpvec2;
    tmpvec = makeOffterm(weight, P2[j], 1, ncat, j);
    tmpvec2 = makeOffterm(weight, P[j], 1, ncat, j);
    double offterm2 = tmpvec - tmpvec2;

    out[akrow + nfact + j - 1] +=
      area * (dat_num[j]*(aTheta2*P[j] - 3*aTheta2*P2[j] +
			  2*aTheta2*P3[j])*numsum - weight[j]/num[j] *
	      (aTheta*P[j] - aTheta*P2[j])*numsum*aTheta + weight[j] *
	      (aTheta*P[j] - aTheta*P2[j])*aTheta + offterm);

    out[dkrow + nfact + ncat + j - 2] +=
      area * (dat_num[j]*(P[j] - 3*P2[j] + 2*P3[j])*numsum - weight[j]/num[j] *
	      (P[j] - P2[j])*numsum + weight[j] *
	      (P[j] - P2[j]) + offterm2);

    for(int i = 1; i < ncat; i++) {
      if(j > i) {
	offterm = makeOffterm2(weight, P[j], P[i], aTheta2, ncat, i);
	offterm2 = makeOffterm2(weight, P[j], P[i], 1, ncat, i);
	tmpvec = dat_num[i] * (-aTheta2*P[i]*P[j] + 2*P2[i] *aTheta2*P[j])*numsum + 
	  dat_num[i] * (aTheta*P[i] - P2[i] * aTheta)*aTheta*num[j]+offterm;
	tmpvec2 = dat_num[i] * (-P[i]*P[j] + 2*P2[i] *P[j]) * numsum +
	  dat_num[i] * (P[i] - P2[i]) * num[j] + offterm2;
	out[akrow + nfact + i - 1] += area * tmpvec;
	out[dkrow + nfact + ncat + i - 2] += area * tmpvec2;
      }
      if (abs(j-i) == 0) {
	tmpvec = makeOffterm(weight, P2[i], aTheta, ncat, i);
	tmpvec2 = makeOffterm(weight, P[i], aTheta, ncat, i);
	offterm = tmpvec - tmpvec2;
	tmpvec = dat_num[i]*(aTheta*P[i] - 3*aTheta*P2[i] +
			     2*aTheta*P3[i]) * numsum - dat_num[i] *
	  (aTheta*P[i] - aTheta*P2[i])*numsum + weight[i] *
	  (P[i] - P2[i])*aTheta + offterm;
	out[dkrow + nfact + i - 1] += area * tmpvec;
      } else {
	offterm = makeOffterm2(weight, P[j], P[i], aTheta, ncat, i);
	tmpvec = dat_num[i] * (-aTheta*P[i]*P[j] + 2*P2[i] *aTheta*P[j]) * numsum + 
	  dat_num[i] * (P[i] - P2[i]) * aTheta * num[j] + offterm;
	out[dkrow + nfact + i - 1] += area * tmpvec;
      }
    }
  }
}

static void
pda(const double *ar, int rows, int cols) {   // column major order
	for (int rx=0; rx < rows; rx++) {
		for (int cx=0; cx < cols; cx++) {
			Rprintf("%.6g, ", ar[cx * rows + rx]);
		}
		Rprintf("\n");
	}
}

static void
irt_rpf_nominal_deriv2(const double *spec,
		       const double *restrict param,
		       double *out)
{
  int nfact = spec[RPF_ISpecDims];
  int nzeta = spec[RPF_ISpecOutcomes] - 1;
  const double *aa = param;

  for (int dx=0; dx < nfact; dx++) {
    if (aa[dx] < 0) {
      set_deriv_nan(spec, out);
      return;
    }
  }

  const double *Ta = spec + RPF_ISpecCount;
  const double *Tc = spec + RPF_ISpecCount + nzeta * nzeta;
  const int numParam = irt_rpf_nominal_numParam(spec);
  double rawOut[numParam];
  memcpy(rawOut, out, sizeof(double) * numParam);

  // gradient
  for (int tx=0; tx < nzeta; tx++) {
    double ak1=0;
    double ck1=0;
    for (int kx=0; kx < nzeta; kx++) {
      int Tcell = tx * nzeta + kx;
      ak1 += rawOut[nfact + kx] * Ta[Tcell];
      ck1 += rawOut[nfact + nzeta + kx] * Tc[Tcell];
    }
    out[nfact + tx] = ak1;
    out[nfact + nzeta + tx] = ck1;
  }

  // don't need to transform the main a parameters TODO
  double *dmat = Realloc(NULL, 3 * numParam * numParam, double);
  const int hsize = hessianIndex(0, numParam-1, numParam-1);
  {
    int row=0;
    int col=0;
    for (int dx=0; dx <= hsize; dx++) {
      dmat[numParam * col + row] = out[numParam + dx];
      if (row == col) {
	col=0; ++row;
      } else {
	dmat[numParam * row + col] = out[numParam + dx];
	++col;
      }
    }
  }

  double *tmat = dmat + numParam * numParam;
  for (int dx=0; dx < numParam * numParam; dx++) tmat[dx] = 0;
  for (int dx=0; dx < nfact; dx++) {
    tmat[dx * numParam + dx] = 1;
  }
  for (int rx=0; rx < nzeta; rx++) {
    for (int cx=0; cx < nzeta; cx++) {
      tmat[(cx + nfact)*numParam + nfact + rx] = Ta[rx * nzeta + cx];
      tmat[(cx + nfact + nzeta)*numParam + nfact + nzeta + rx] = Tc[rx * nzeta + cx];
    }
  }

  double *dest = dmat + 2 * numParam * numParam;

  // It is probably possible to do this more efficiently than dgemm
  // since we know that we only care about the lower triangle.
  // I'm not sure whether this is worth optimizing. TODO

  char normal = 'n';
  char transpose = 't';
  double one = 1;
  double zero = 0;
  F77_CALL(dgemm)(&normal, &normal, &numParam, &numParam, &numParam,
		  &one, tmat, &numParam, dmat, &numParam, &zero, dest, &numParam);
  F77_CALL(dgemm)(&normal, &transpose, &numParam, &numParam, &numParam,
		  &one, dest, &numParam, tmat, &numParam, &zero, dmat, &numParam);

  {
    int row=0;
    int col=0;
    for (int dx=0; dx <= hsize; dx++) {
      out[numParam + dx] = dmat[numParam * col + row];
      if (row == col) {
	col=0; ++row;
      } else {
	++col;
      }
    }
  }

  Free(dmat);
}

static int
irt_rpf_1dim_gpcm_numSpec(const double *spec)
{ return RPF_ISpecCount; }

static int
irt_rpf_1dim_gpcm_numParam(const double *spec)
{ return spec[RPF_ISpecOutcomes]; }

static void
irt_rpf_1dim_gpcm_prob(const double *spec,
		       const double *restrict param, const double *restrict th,
		       double *restrict out)
{
  int numOutcomes = spec[RPF_ISpecOutcomes];
  double discr = param[0];
  double term1[numOutcomes];

  term1[numOutcomes - 1] = 0;

  for (int tx=0; tx < numOutcomes-1; tx++) {
    term1[tx] = -discr * (*th - param[tx+1]);
  }
  double sum = 0;
  double denom = 0;
  double term2[numOutcomes];
  for (int tx=numOutcomes-1; tx >= 0; tx--) {
    sum += term1[tx];
    double safe_sum;
    if (sum < -EXP_STABLE_DOMAIN) safe_sum = -EXP_STABLE_DOMAIN;
    else if (sum > EXP_STABLE_DOMAIN) safe_sum = EXP_STABLE_DOMAIN;
    else safe_sum = sum;
    term2[tx] = safe_sum;
    denom += exp(safe_sum);
  }
  for (int tx=0; tx < numOutcomes; tx++) {
    out[tx] = exp(term2[tx]) / denom;
  }
}

static double
irt_rpf_1dim_gpcm_prior(const double *spec,
			const double *restrict param)
{
  //const double *prior = spec + RPF_ISpecCount;
  return 0; //lognormal_pdf(param[0], prior[0]);
}

/**
 * Based on Muraki (1992, p. 167). I am not sure exactly how the
 * parameterization here relates to the parameterization in Muraki (1992).
 *
 * GPCM(3)
 * irf1(a,b1,b2,th) := 1/(exp(-a*(th-b2) + -a*(th-b1))+exp(-a*(th-b2))+ 1);
 * irf2(a,b1,b2,th) := (exp(-a*(th-b2)))/(exp(-a*(th-b2) + -a*(th-b1))+exp(-a*(th-b2))+ 1);
 * irf3(a,b1,b2,th) := (exp(-a*(th-b2) + -a*(th-b1)))/(exp(-a*(th-b2) + -a*(th-b1))+exp(-a*(th-b2))+ 1);
 * plot2d([irf1(1.5,-1,1,th), irf2(1.5,-1,1,th), irf3(1.5,-1,1,th)],[th,-3,3]);
 *
 * Muraki (1992) reparameterization?
 * P_0 irf1(a,b,d2,d1,th) := 1/(exp(-a*(th-b+d1) + -a*(th-b+d2))+exp(-a*(th-b+d1))+ 1)
 * P_1 irf2(a,b,d2,d1,th) := (exp(-a*(th-b+d1)))/(exp(-a*(th-b+d1) + -a*(th-b+d2))+exp(-a*(th-b+d1))+ 1)
 * P_2 irf3(a,b,d2,d1,th) := (exp(-a*(th-b+d1) + -a*(th-b+d2)))/(exp(-a*(th-b+d1) + -a*(th-b+d2))+exp(-a*(th-b+d1))+ 1)
 * plot2d([irf1(1.5,-1,0,-2,th), irf2(1.5,-1,0,-2,th), irf3(1.5,-1,0,-2,th)],[th,-3,3]);
 *
 * -b1 = -b+d2
 * -b2 = -b+d1
 */

static double
_1dim_gpcm_z(const int kk, const double *restrict param, double th)
{
  double sum=0;
  for (int vv=1; vv < kk; vv++) {
    sum += th - param[vv];
  }
  return param[0] * sum;
}

static void
irt_rpf_1dim_gpcm_deriv1(const double *spec,
			 const double *restrict param,
			 const double *where, const double area,
			 const double *weight, double *out)
{
  int numOutcomes = spec[RPF_ISpecOutcomes];
  double pout[numOutcomes];
  irt_rpf_1dim_gpcm_prob(spec, param, where, pout);

  double th = where[0];

  {
    double grad=0;
    for (int kx=1; kx <= numOutcomes; kx++) {
      double sum=0;
      for (int cc=1; cc <= numOutcomes; cc++) {
	sum += _1dim_gpcm_z(cc, param, th) * pout[cc-1];
      }
      grad += weight[kx-1] * (_1dim_gpcm_z(kx, param, th) - sum);
    }
    out[0] += area * grad;
  }
  double total_weight=0;
  for (int cc=0; cc < numOutcomes; cc++) {
    total_weight += weight[cc];
  }
  for (int bx=1; bx < numOutcomes; bx++) {
    double grad = 0;
    for (int kx=0; kx <= bx-1; kx++) {
      grad += weight[kx] - pout[kx] * total_weight;
    }
    out[bx] += area * grad;
  }
}

static void
irt_rpf_1dim_gpcm_deriv2(const double *spec,
			 const double *restrict param,
			 double *out)
{
  int numOutcomes = spec[RPF_ISpecOutcomes];
  double aa = param[0];
  if (aa <= 0) {
    set_deriv_nan(spec, out);
    return;
  }
  //const double *prior = spec + RPF_ISpecCount;
  out[0] /= aa;
  //out[0] += lognormal_gradient(aa, prior[0]);
  for (int bx=1; bx < numOutcomes; bx++) {
    out[bx] *= aa;
  }
  const int numH = numOutcomes * (numOutcomes+1) / 2;
  for (int px=numOutcomes; px < numOutcomes + numH; px++) out[px] = nan("I");
}

static void
irt_rpf_1dim_gpcm_rescale(const double *spec, double *restrict param, const int *paramMask,
			  const double *restrict mean, const double *restrict choleskyCov)
{
  int numOutcomes = spec[RPF_ISpecOutcomes];
  double thresh[numOutcomes-1];
  for (int tx=0; tx < numOutcomes-1; tx++) {
    thresh[tx] = param[tx+1] * -param[0];
  }
  if (paramMask[0] >= 0) {
    param[0] *= choleskyCov[0];
  }
  double shift = param[0] * mean[0];
  for (int tx=0; tx < numOutcomes-1; tx++) {
    if (paramMask[tx+1] < 0) continue;
    thresh[tx] += shift;
    param[tx+1] = thresh[tx] / -param[0];
  }
}

static void noop() {}

const struct rpf librpf_model[] = {
  { "drm1-",
    irt_rpf_1dim_drm_numSpec,
    irt_rpf_1dim_drm_numParam,
    irt_rpf_1dim_drm_prob,
    irt_rpf_logprob_adapter,
    irt_rpf_1dim_drm_prior,
    irt_rpf_1dim_drm_deriv1,
    irt_rpf_1dim_drm_deriv2,
    irt_rpf_1dim_drm_rescale,
    noop,
    noop,
  },
  { "drm1",
    irt_rpf_1dim_drm_numSpec,
    irt_rpf_1dim_drm_numParam,
    irt_rpf_1dim_drm_prob,
    irt_rpf_logprob_adapter,
    0,
    0,
    0,
    0,
    irt_rpf_1dim_drm_prefit,
    noop,
  },
  { "drm1+",
    irt_rpf_mdim_drm_numSpec,
    irt_rpf_mdim_drm_numParam,
    irt_rpf_mdim_drm_prob,
    irt_rpf_logprob_adapter,
    irt_rpf_mdim_drm_prior,
    irt_rpf_mdim_drm_deriv1,
    irt_rpf_mdim_drm_deriv2,
    irt_rpf_mdim_drm_rescale,
    noop,
    irt_rpf_1dim_drm_postfit,
  },
  { "drm",
    irt_rpf_mdim_drm_numSpec,
    irt_rpf_mdim_drm_numParam,
    irt_rpf_mdim_drm_prob,
    irt_rpf_logprob_adapter,
    irt_rpf_mdim_drm_prior,
    irt_rpf_mdim_drm_deriv1,
    irt_rpf_mdim_drm_deriv2,
    irt_rpf_mdim_drm_rescale,
    noop,
    noop
  },
  { "drm+phil",
    irt_rpf_mdim_drm_numSpec,
    irt_rpf_mdim_drm_numParam,
    irt_rpf_mdim_drm_prob,
    irt_rpf_logprob_adapter,
    (rpf_prior_t) noop,
    irt_rpf_mdim_drm_deriv_phil1,
    irt_rpf_mdim_drm_deriv_phil2,
    irt_rpf_mdim_drm_rescale,
    noop,
    noop
  },
  { "gpcm1",
    irt_rpf_1dim_gpcm_numSpec,
    irt_rpf_1dim_gpcm_numParam,
    irt_rpf_1dim_gpcm_prob,
    irt_rpf_logprob_adapter,
    irt_rpf_1dim_gpcm_prior,
    irt_rpf_1dim_gpcm_deriv1,
    irt_rpf_1dim_gpcm_deriv2,
    irt_rpf_1dim_gpcm_rescale,
    noop,
    noop
  },
  { "grm1",
    irt_rpf_mdim_grm_numSpec,
    irt_rpf_mdim_grm_numParam,
    irt_rpf_1dim_grm_prob,
    irt_rpf_logprob_adapter,
    (rpf_prior_t) noop,
    noop,
    noop,
    noop,
    noop,
    noop
  },
  { "grm",
    irt_rpf_mdim_grm_numSpec,
    irt_rpf_mdim_grm_numParam,
    irt_rpf_mdim_grm_prob,
    irt_rpf_logprob_adapter,
    (rpf_prior_t) noop,
    irt_rpf_mdim_grm_deriv1,
    irt_rpf_mdim_grm_deriv2,
    noop, // rescale
    noop,
    noop
  },
  { "nominal",
    irt_rpf_nominal_numSpec,
    irt_rpf_nominal_numParam,
    irt_rpf_nominal_prob,
    irt_rpf_nominal_logprob,
    (rpf_prior_t) noop,
    irt_rpf_nominal_deriv1,
    irt_rpf_nominal_deriv2,
    noop, // rescale
    noop,
    noop
  }
};

const int librpf_numModels = (sizeof(librpf_model) / sizeof(struct rpf));
