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

#ifndef _LIBIRT_RPF_
#define _LIBIRT_RPF_

enum RPF_ISpec {
  RPF_ISpecID,
  RPF_ISpecOutcomes,
  RPF_ISpecDims,
  RPF_ISpecCount
};

#define RPF_ISpecFacts RPF_ISpecDims

typedef int (*rpf_numSpec_t)(const double *spec);
typedef int (*rpf_numParam_t)(const double *spec);
typedef void (*rpf_prob_t)(const double *spec,
			   const double *restrict param, const double *restrict th,
			   double *restrict out);
typedef double (*rpf_prior_t)(const double *spec,
			      const double *restrict param);
typedef void (*rpf_dLL1_t)(const double *spec,
			   const double *restrict param,
			   const double *where, const double area,
			   const double *weight, double *out);
typedef void (*rpf_dLL2_t)(const double *spec, const double *restrict param, double *out);
typedef void (*rpf_rescale_t)(const double *spec, double *restrict param, const int *paramMask,
			      const double *restrict mean, const double *restrict choleskyCov);
typedef void (*rpf_transform_t)(double *spec, double *param);

struct rpf {
  const char name[10];
  rpf_numSpec_t numSpec;
  rpf_numParam_t numParam;
  rpf_prob_t prob;
  rpf_prob_t logprob;
  rpf_prior_t prior;
  rpf_dLL1_t dLL1;
  rpf_dLL2_t dLL2;
  rpf_rescale_t rescale;
  rpf_transform_t prefit;
  rpf_transform_t postfit;
};

/* R_GetCCallable */
typedef void (*get_librpf_t)(int *version, int *numModels, const struct rpf **model);

extern const struct rpf librpf_model[];
extern const int librpf_numModels;

#endif
