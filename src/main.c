#include "argparse.h"   // command line parser
#include "iio.h"        // image i/o

#include <assert.h>     // assert
#include <stdlib.h>     // NULL, EXIT_SUCCESS/FAILURE
#include <stdio.h>      // printf, fprintf, stderr
#include <math.h>       // expf, nans (used as boundary value by bicubic interp)

// some macros and data types [[[1

//#define DUMP_INFO

// comment for a simpler version without keeping track of pixel variances
//#define VARIANCES

// comment for uniform aggregation
#define WEIGHTED_AGGREGATION

// this enables a Kalman type of temporal recursion
#define KALMAN_RECURSION

// transition variance is estimated via temporal patch similarity
#define AGGREGATE_TRANSITION_VAR

#define FLT_HUGE 1e10

// read/write image sequence [[[1
static
float * vio_read_video_float_vec(const char * const path, int first, int last, 
		int *w, int *h, int *pd)
{
	// retrieve size from first frame and allocate memory for video
	int frames = last - first + 1;
	int whc;
	float *vid = NULL;
	{
		char frame_name[512];
		sprintf(frame_name, path, first);
		float *im = iio_read_image_float_vec(frame_name, w, h, pd);

		// size of a frame
		whc = *w**h**pd;

		vid = malloc(frames*whc*sizeof(float));
		memcpy(vid, im, whc*sizeof(float));
		if(im) free(im);
	}

	// load video
	for (int f = first + 1; f <= last; ++f)
	{
		int w1, h1, c1;
		char frame_name[512];
		sprintf(frame_name, path, f);
		float *im = iio_read_image_float_vec(frame_name, &w1, &h1, &c1);

		// check size
		if (whc != w1*h1*c1)
		{
			fprintf(stderr, "Size missmatch when reading frame %d\n", f);
			if (im)  free(im);
			if (vid) free(vid);
			return NULL;
		}

		// copy to video buffer
		memcpy(vid + (f - first)*whc, im, whc*sizeof(float));
		if(im) free(im);
	}

	return vid;
}

static
void vio_save_video_float_vec(const char * const path, float * vid, 
		int first, int last, int w, int h, int c)
{
	const int whc = w*h*c;
	for (int f = first; f <= last; ++f)
	{
		char frame_name[512];
		sprintf(frame_name, path, f);
		float * im = vid + (f - first)*whc;
		iio_save_image_float_vec(frame_name, im, w, h, c);
	}
}


// bicubic interpolation [[[1

#ifdef NAN
// extrapolate by nan
inline static
float getsample_nan(float *x, int w, int h, int pd, int i, int j, int l)
{
	assert(l >= 0 && l < pd);
	return (i < 0 || i >= w || j < 0 || j >= h) ? NAN : x[(i + j*w)*pd + l];
}
#endif//NAN

inline static
float cubic_interpolation(float v[4], float x)
{
	return v[1] + 0.5 * x*(v[2] - v[0]
			+ x*(2.0*v[0] - 5.0*v[1] + 4.0*v[2] - v[3]
			+ x*(3.0*(v[1] - v[2]) + v[3] - v[0])));
}

static
float bicubic_interpolation_cell(float p[4][4], float x, float y)
{
	float v[4];
	v[0] = cubic_interpolation(p[0], y);
	v[1] = cubic_interpolation(p[1], y);
	v[2] = cubic_interpolation(p[2], y);
	v[3] = cubic_interpolation(p[3], y);
	return cubic_interpolation(v, x);
}

static
void bicubic_interpolation_nans(float *result,
		float *img, int w, int h, int pd, float x, float y)
{
	x -= 1;
	y -= 1;

	int ix = floor(x);
	int iy = floor(y);
	for (int l = 0; l < pd; l++) {
		float c[4][4];
		for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++)
			c[i][j] = getsample_nan(img, w, h, pd, ix + i, iy + j, l);
		float r = bicubic_interpolation_cell(c, x - ix, y - iy);
		result[l] = r;
	}
}

static
void warp_bicubic_inplace(float *imw, float *im, float *of, int w, int h, int ch)
{
	// warp previous frame
	for (int y = 0; y < h; ++y)
	for (int x = 0; x < w; ++x)
	{
		float xw = x + of[(x + y*w)*2 + 0];
		float yw = y + of[(x + y*w)*2 + 1];
		bicubic_interpolation_nans(imw + (x + y*w)*ch, im, w, h, ch, xw, yw);
	}
	return;
}

/* static
float * warp_bicubic(float * im, float * of, int w, int h, int ch)
{
	// warp previous frame
	float * im_w = malloc(w*h*ch * sizeof(float));
	warp_bicubic_inplace(im_w, im, of, w, h, ch);
	return im_w;
} */

// recursive nl-means parameters [[[1

// struct for storing the parameters of the algorithm
struct vnlmeans_params
{
	int patch_sz;        // patch size
	int search_sz;       // search window radius
	float weights_hx;    // spatial patch similarity weights parameter
	float weights_hd;    // spatial distance weights parameter (for bilateral)
	float weights_thx;   // spatial weights threshold
	float weights_ht;    // temporal patch similarity weights parameter
	float weights_htv;   // transition variance weights parameter
	float dista_lambda;  // weight of current frame in patch distance
	float tv_lambda;     // weight of current frame in patch distance
	bool pixelwise;      // toggle pixel-wise nlmeans
};

// set default parameters as a function of sigma
void vnlmeans_default_params(struct vnlmeans_params * p, float sigma)
{
	const bool a = !(p->pixelwise); // set by caller

	// set patch size first
	if (p->patch_sz < 0) p->patch_sz = a ? 8 : 5;

	switch (p->patch_sz)
	{
		case 1:
			// the parameters are based on the following parameters
			// found with a parameter search:
			//
			//           psz wsz hx   hd  ht   hv ltv  lx
			// sigma 10:  1   10 10.2 4    7.5 0  0.5  0.1 
			// sigma 20:  1   10 24.4 1.6 14.1 0  0.3  0.1
			// sigma 40:  1   10 48.0 1.6 27.1 0  0.6  0.02
			if (p->weights_hx   < 0) p->weights_hx   = 1.2 * sigma;
			if (p->weights_hd   < 0) p->weights_hd   = 1.6; 
			if (p->search_sz    < 0) p->search_sz    = 3*p->weights_hd;
			if (p->weights_thx  < 0) p->weights_thx  = .05f;
			if (p->weights_ht   < 0) p->weights_ht   = 0.7 * sigma;
			if (p->weights_htv  < 0) p->weights_htv  = 0;
			if (p->tv_lambda    < 0) p->tv_lambda    = 0.5;
			if (p->dista_lambda < 0) 
				p->dista_lambda = fmax(0, fmin(0.2, 0.1 - (sigma - 20)/400));

			// limit search region to prevent too long running times
			p->search_sz = fmin(15, fmin(3*p->weights_hd, p->search_sz));

			break;

		case 4: // TODO
		case 8:
		default: // these are the automatic params for a patch size of 8
			if (p->search_sz    < 0) p->search_sz    = 10;
			if (p->weights_hx   < 0) p->weights_hx   = 0.85 * sigma;
			if (p->weights_hd   < 0) p->weights_hd   = FLT_HUGE;
			if (p->weights_thx  < 0) p->weights_thx  = .05f;
			if (p->weights_ht   < 0) p->weights_ht   = 1.35 * sigma;
			if (p->weights_htv  < 0) p->weights_htv  = 1.35 * sigma;
			if (p->dista_lambda < 0) p->dista_lambda = 1.;
			if (p->tv_lambda    < 0) p->tv_lambda    = 0.85;
	}
}

// recursive nl-means for frame t (pixel temporal average) [[[1
void vnlmeans_kalman_frame(float *deno1, float *nisy1, float *deno0,
#ifdef VARIANCES
		float *vari1, float *vari0,
#endif
		int w, int h, int ch, float sigma,
		const struct vnlmeans_params prms, int frame)
{
	// definitions [[[2

	const int psz = prms.patch_sz;
	const int step = prms.pixelwise ? 1 : fmax(1, psz/2);
	const float weights_hx2  = prms.weights_hx * prms.weights_hx;
	const float weights_hd2  = prms.weights_hd * prms.weights_hd * 2;
	const float weights_ht2  = prms.weights_ht * prms.weights_ht;
	const float weights_htv2 = prms.weights_htv * prms.weights_htv;
	const float sigma2 = sigma * sigma;


	// aggregation weights (not necessary for pixel-wise nlmeans)
	float *aggr1 = prms.pixelwise ? NULL : malloc(w*h*sizeof(float));

#ifdef AGGREGATE_TRANSITION_VAR
	/* Transition variances are estimated as a weighted average of the
	 * transition l2 error of all patches that overlap a pixel.  In practice,
	 * this means that we compute for all patches the squared l2 error between
	 * the current patch and its previous version. We compute a Gaussian
	 * similarity weight with parameter weight_hvt, and aggregate these weighted
	 * variances on a variance image. This works as a soft min of the l2 errors
	 * among all patches overlapping a pixel. If weight_hvt = 0 the soft min
	 * converges to a min. */
	float *var01 = prms.pixelwise ? NULL : malloc(w*h*sizeof(float));
	float *agg01 = prms.pixelwise ? NULL : malloc(w*h*sizeof(float));
	if (agg01) for (int i = 0; i < w*h; ++i) agg01[i] = 0.;
	if (var01) for (int i = 0; i < w*h; ++i) var01[i] = 0.;
#endif

	// set output and aggregation weights to 0
	for (int i = 0; i < w*h*ch; ++i) deno1[i] = 0.;
	if (aggr1) for (int i = 0; i < w*h; ++i) aggr1[i] = 0.;

#ifdef VARIANCES
	// set variances to 0
	for (int i = 0; i < w*h; ++i) vari1[i] = 0.;
#endif

	// noisy and clean patches at point p (as VLAs in the stack!)
	float N1[psz][psz][ch]; // noisy patch at position p in frame t
	float D1[psz][psz][ch]; // denoised patch at p in frame t (target patch)
	float D0[psz][psz][ch]; // denoised patch at p in frame t - 1
#if defined(WEIGHTED_AGGREGATION) || defined(VARIANCES) || defined(_OPENMP)
	float V1[psz][psz];     // patch variance after the spatial average
#endif

	// wrap images with nice pointers to vlas
	float (*a1)[w]     = (void *)aggr1;       // aggregation weights at t
	float (*d1)[w][ch] = (void *)deno1;       // denoised frame t (output)
	const float (*d0)[w][ch] = (void *)deno0; // denoised frame t-1
	const float (*n1)[w][ch] = (void *)nisy1; // noisy frame at t
#ifdef VARIANCES
	float (*v0)[w] = (void *)vari0;       // aggregation weights at t
	float (*v1)[w] = (void *)vari1;       // aggregation weights at t
#endif
#ifdef AGGREGATE_TRANSITION_VAR
	float (*a01)[w] = (void *)agg01;
	float (*v01)[w] = (void *)var01;
#endif

	// spatial denoising for frame t [[[2
	for (int oy = 0; oy < psz; oy += step) // split in grids of non-overlapping
	for (int ox = 0; ox < psz; ox += step) // patches (for parallelization)
	#pragma omp parallel for private(N1,D1,D0,V1)
	for (int py = oy; py < h - psz + 1; py += psz) // FIXME: boundary pixels
	for (int px = ox; px < w - psz + 1; px += psz) // may not be denoised
	{
		/* print state of iteration
		if ((py * w * step) % 10*step == 0) printf("%4d\b\b\b\b", py, px);*/

		//	load target patch [[[3
		for (int hy = 0; hy < psz; ++hy)
		for (int hx = 0; hx < psz; ++hx)
		for (int c  = 0; c  < ch ; ++c )
		{
			if (d0) D0[hy][hx][c] = d0[py + hy][px + hx][c];
			N1[hy][hx][c] = n1[py + hy][px + hx][c];
			D1[hy][hx][c] = 0;
		}

		// spatial average: loop on search region [[[3
		float cp = 0.; // sum of similarity weights, used for normalization
		if (weights_hx2)
		{
			const int wsz = prms.search_sz;
			const int wx[2] = {fmax(px - wsz, 0), fmin(px + wsz, w - psz) + 1};
			const int wy[2] = {fmax(py - wsz, 0), fmin(py + wsz, h - psz) + 1};
#ifdef TRIM_SELF_WEIGHT
			float maxw = 0.;
#endif
			for (int qy = wy[0]; qy < wy[1]; ++qy)
			for (int qx = wx[0]; qx < wx[1]; ++qx)
#ifdef TRIM_SELF_WEIGHT
			if (qx != px && qy != py)
#endif
			{
				// compute patch distance [[[4
				float ww = 0;
				const float l = prms.dista_lambda;
				for (int hy = 0; hy < psz; ++hy)
				for (int hx = 0; hx < psz; ++hx)
					if (d0 && l != 1 && 
					    !isnan(d0[qy + hy][qx + hx][0]) && !isnan(D0[hy][hx][0]))
						// use noisy and denoised patches from previous frame
						for (int c  = 0; c  < ch ; ++c )
						{
							const float eN1 = n1[qy + hy][qx + hx][c] - N1[hy][hx][c];
							const float eD0 = d0[qy + hy][qx + hx][c] - D0[hy][hx][c];
							ww += l * eN1 * eN1 + (1 - l) * eD0 * eD0;
//							ww += l * fmax(eN1*eN1 - 2*sigma2, 0.) + (1 - l) * eD0*eD0;
						}
					else
						// use only noisy from previous frame
						for (int c  = 0; c  < ch ; ++c )
						{
							const float eN1 = n1[qy + hy][qx + hx][c] - N1[hy][hx][c];
//							ww += fmax(eN1 * eN1 - 2*sigma2, 0.);
							ww += eN1 * eN1;
						}

				// compute spatial similarity weight ]]]4[[[4
				if (weights_hx2)
					ww = expf(-1 / weights_hx2 * ww / (float)(psz*psz*ch));
//					ww = (ww / (float)(psz*psz*ch)) < weights_hx2;
//					ww = 1.;
				else
					ww = (qx == px && qy == py) ? 1. : 0.;

				if (weights_hd2 < FLT_HUGE)
				{
					if (weights_hd2)
					{
						const float dx = (px - qx), dy = (py - qy);
						ww *= expf(-1 / weights_hd2 * (dx * dx + dy * dy) );
					}
					else
						ww = (qx == px && qy == py) ? 1. : 0.;
				}

#ifdef TRIM_SELF_WEIGHT
				maxw = fmax(ww, maxw);
#endif
				// accumulate on output patch ]]]4[[[4
				if (ww > prms.weights_thx)
				{
					cp += ww;
					for (int hy = 0; hy < psz; ++hy)
					for (int hx = 0; hx < psz; ++hx)
					for (int c  = 0; c  < ch ; ++c )
						D1[hy][hx][c] += n1[qy + hy][qx + hx][c] * ww;
				}// ]]]4
			}
#ifdef TRIM_SELF_WEIGHT
			// accumulate the central patch using maxw
			maxw = (maxw > 1e-3) ? maxw : 1.;
			cp += maxw;
			for (int hy = 0; hy < psz; ++hy)
			for (int hx = 0; hx < psz; ++hx)
			for (int c  = 0; c  < ch ; ++c )
				D1[hy][hx][c] += n1[py + hy][px + hx][c] * maxw;
#endif
		}
		else
		{
			// copy noisy patch to output
			cp = 1.;
			for (int hy = 0; hy < psz; ++hy)
			for (int hx = 0; hx < psz; ++hx)
			for (int c  = 0; c  < ch ; ++c )
				D1[hy][hx][c] = n1[py + hy][px + hx][c];
		}

		// normalize spatial average [[[3
		float icp = 1. / fmax(cp, 1e-6);
		for (int hy = 0; hy < psz; ++hy)
		for (int hx = 0; hx < psz; ++hx)
		for (int c  = 0; c  < ch ; ++c )
			D1[hy][hx][c] *= icp;

#if defined(WEIGHTED_AGGREGATION) || defined(VARIANCES)
		// TODO: it is unnecessary to have a patch V1 of variances
		// variance of D1
		float vp = sigma2 / cp;
		for (int hy = 0; hy < psz; ++hy)
		for (int hx = 0; hx < psz; ++hx)
			V1[hy][hx] = vp;
#endif

		// compute transition variance [[[3
#ifdef AGGREGATE_TRANSITION_VAR
		float tvp = 0.; // transition variance of patch p
		float twp = 0.; // associated similarity weight
		if (d0)
		{
			int n = 0; // count valid pixels
			const float l01 = prms.tv_lambda;
			for (int hy = 0; hy < psz; ++hy)
			for (int hx = 0; hx < psz; ++hx)
				if (!isnan(D0[hy][hx][0]))
				{
					for (int c  = 0; c  < ch; ++c)
					{
						/* NOTE: the computation of the error between d0 and the spatially
						 * denoised d1 could (and should?) be done after spatial aggregation.
						 * That requires a local loop with a patch kernel to compute all
						 * patch transition errors. A simpler alternative is to just compute
						 * the pixel-wise error between d0 and the aggregated d1. */
						const float eD = D1[hy][hx][c] - D0[hy][hx][c];
						const float eN = N1[hy][hx][c] - D0[hy][hx][c];
						tvp += l01 * fmax(eN * eN - sigma2, 0.f) + (1 - l01) * eD * eD;
					}
					n++;
				}

			// normalize variance estimate
			tvp /= (float)(n*ch);

			// compute similarity weight
			if (weights_htv2 && n)
				twp = expf(-1 / weights_htv2 * tvp );
			else
				twp = 0;
		}
#endif

		// aggregate denoised patch on spatial output image [[[3
		if (a1)
			// patch-wise denoising: aggregate the whole denoised patch
			for (int hy = 0; hy < psz; ++hy)
			for (int hx = 0; hx < psz; ++hx)
			{
#ifdef WEIGHTED_AGGREGATION
				// weighted aggregation by inverse variance
				const float ww = 1.f/V1[hy][hx];
#else
				// uniform aggregation
				const float ww = 1.f;
#endif
				// aggregation weights and denoised image
				a1[py + hy][px + hx] += ww;
				for (int c = 0; c < ch ; ++c )
					d1[py + hy][px + hx][c] += D1[hy][hx][c] * ww;

#ifdef VARIANCES
				// aggregate variances as well
				v1[py + hy][px + hx] += ww * ww * V1[hy][hx];
#endif

#ifdef AGGREGATE_TRANSITION_VAR
				if (weights_htv2)
				{
					// aggregate on v01 as a soft-min
					a01[py + hy][px + hx] += twp;
					v01[py + hy][px + hx] += twp * tvp;
				}
				else
				{
					// aggregate on v01 as a hard-min
					if (a01[py + hy][px + hx])
						v01[py + hy][px + hx] = fmin(v01[py + hy][px + hx], tvp);
					else
					{
						a01[py + hy][px + hx] = 1.;
						v01[py + hy][px + hx] = tvp;
					}
				}
#endif
			}
		else 
		{
			// pixel-wise denoising: aggregate only the central pixel
			for (int c = 0; c < ch ; ++c )
				d1[py + psz/2][px + psz/2][c] += D1[psz/2][psz/2][c];
#ifdef VARIANCES
			v1[py + psz/2][px + psz/2] = vp * vp;
#endif
		}
		// ]]]3
	}

	// normalize spatial output image [[[3
	if (aggr1)
	for (int i = 0, j = 0; i < w*h; ++i) 
	for (int c = 0; c < ch ; ++c, ++j) 
		deno1[j] /= aggr1[i];

#ifdef VARIANCES
	if (aggr1)
	for (int i = 0; i < w*h; ++i) 
		vari1[i] /= aggr1[i] * aggr1[i];
#endif

#ifdef DUMP_INFO
	if (d0)
	{
		char name[512];
		sprintf(name, "dump/vari1.%03d.tif",frame);
		iio_save_image_float_vec(name, vari1, w, h, 1);

//	#ifdef AGGREGATE_TRANSITION_VAR
//		sprintf(name, "dump/var01.%03d.tif",frame);
//		iio_save_image_float_vec(name, var01, w, h, 1);
//	#endif
	}
#endif

	// temporal average with frame t-1 [[[2
	if (d0)
	{
		#pragma omp parallel for
		for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		if (!isnan(d0[y][x][0]))
		{
#ifdef AGGREGATE_TRANSITION_VAR
			// transition variance computed by aggregation
			const float v01xy = v01[y][x] / (a01 ? a01[y][x] : 1.);
#else
			// estimate transition variance as (d0 - d1)^2
			float v01xy = 0.;
			for (int c = 0; c < ch; ++c)
			{
				const float e = d0[y][x][c] - d1[y][x][c];
				v01xy += e * e / (float)ch;
			}
#endif
			// compute variance multiplier
			const float w01xy = fmin(1, fmax(0, expf( - 1./weights_ht2 * v01xy )));

#ifdef VARIANCES 
			// compute coefficient in convex combination
			const float v0xy = fmax(v0[y][x], 1e-2);
			const float f = fmin(1, fmax(0, v0xy / (v0xy + v1[y][x] * w01xy)));
			
			// update pixel variance
			v1[y][x] = fmax(0.f, v0xy * v1[y][x] / fmax(v0xy + v1[y][x] * w01xy, 1e-4));
#else
//			// no variances: we assume that v0 = v1
//			const float f = min(1., fmax(0., 1./(1. + w01xy)));
			// no variances: we assume that v0 = (1 - w01)v1
			const float f = 1. - w01xy;
#endif

			// update pixel value
			for (int c = 0; c < ch; ++c)
				d1[y][x][c] = d0[y][x][c] * (1 - f) + d1[y][x][c] * f;

#ifdef DUMP_INFO
			v01[y][x] = w01xy;
			v0[y][x] = f;
#endif
		}
		else
		{
#ifdef DUMP_INFO
			v01[y][x] = 0.;
			v0[y][x] = 1.;
#endif
		}
	}

	// clean-up [[[2
#ifdef DUMP_INFO
	if (d0)
	{
		char name[512];
		sprintf(name, "dump/f.%03d.tif",frame);
		iio_save_image_float_vec(name, vari0, w, h, 1);

	#ifdef AGGREGATE_TRANSITION_VAR
		sprintf(name, "dump/w.%03d.tif",frame);
		iio_save_image_float_vec(name, var01, w, h, 1);
	#endif
	}
#endif

#ifdef AGGREGATE_TRANSITION_VAR
//	// for visualization: copy to vari1
//	for (int i = 0; i < w*h; ++i)
//		vari1[i] = var01[i];
#endif

	// free allocated mem and quit
	if (aggr1) free(aggr1);
#ifdef AGGREGATE_TRANSITION_VAR
	if (var01) free(var01);
	if (agg01) free(agg01);
#endif
	return; // ]]]2
}

// recursive nl-means for frame t (patch temporal average) [[[1
void vnlmeans_frame(float *deno1, float *nisy1, float *deno0,
#ifdef VARIANCES
		float *vari1, float *vari0,
#endif
		int w, int h, int ch, float sigma,
		const struct vnlmeans_params prms)
{
	// definitions [[[2

	const int psz = prms.patch_sz;
	const int step = prms.pixelwise ? 1 : psz/2;
	const float weights_hx2  = prms.weights_hx * prms.weights_hx;
	const float weights_ht2  = prms.weights_ht * prms.weights_ht;
	const float weights_htv2 = prms.weights_htv * prms.weights_htv;

	// aggregation weights (not necessary for pixel-wise nlmeans)
	float *aggr1 = prms.pixelwise ? NULL : malloc(w*h*sizeof(float));

	// set output and aggregation weights to 0
	for (int i = 0; i < w*h*ch; ++i) deno1[i] = 0.;
	if (aggr1) for (int i = 0; i < w*h; ++i) aggr1[i] = 0.;

#ifdef VARIANCES
	// set variances to 0
	for (int i = 0; i < w*h; ++i) vari1[i] = 0.;
#endif

	// noisy and clean patches at point p (as VLAs in the stack!)
	float N1[psz][psz][ch]; // noisy patch at position p in frame t
	float D1[psz][psz][ch]; // denoised patch at p in frame t (target patch)
	float D0[psz][psz][ch]; // denoised patch at p in frame t - 1
#if defined(VARIANCES) || defined(_OPENMP)
	float V1[psz][psz];     // patch variance after the spatial average
#endif

	// wrap images with nice pointers to vlas
	float (*a1)[w]     = (void *)aggr1;       // aggregation weights at t
	float (*d1)[w][ch] = (void *)deno1;       // denoised frame t (output)
	const float (*d0)[w][ch] = (void *)deno0; // denoised frame t-1
	const float (*n1)[w][ch] = (void *)nisy1; // noisy frame at t
#ifdef VARIANCES
	float (*v1)[w]     = (void *)vari1;       // aggregation weights at t
#endif

	// loop on image patches [[[2
	for (int oy = 0; oy < psz; oy += step) // split in grids of non-overlapping
	for (int ox = 0; ox < psz; ox += step) // patches (for parallelization)
	#pragma omp parallel for private(N1,D1,D0,V1)
	for (int py = oy; py < h - psz + 1; py += psz)
	for (int px = ox; px < w - psz + 1; px += psz)
	{
		//	load target patch [[[3
		for (int hy = 0; hy < psz; ++hy)
		for (int hx = 0; hx < psz; ++hx)
		for (int c  = 0; c  < ch ; ++c )
		{
			if (d0) D0[hy][hx][c] = d0[py + hy][px + hx][c];
			N1[hy][hx][c] = n1[py + hy][px + hx][c];
			D1[hy][hx][c] = 0;
		}

		// spatial average: loop on search region [[[3
		float cp = 0.; // sum of all weights (for normalization)
		if (weights_hx2)
		{
			const int wsz = prms.search_sz;
			const int wx[2] = {fmax(px - wsz, 0), fmin(px + wsz, w - psz) + 1};
			const int wy[2] = {fmax(py - wsz, 0), fmin(py + wsz, h - psz) + 1};
			for (int qy = wy[0]; qy < wy[1]; ++qy)
			for (int qx = wx[0]; qx < wx[1]; ++qx)
			{
				// compute patch distance [[[4
				float ww = 0; // patch distance is saved here
				const float l = prms.dista_lambda;
				for (int hy = 0; hy < psz; ++hy)
				for (int hx = 0; hx < psz; ++hx)
					if (d0 && l != 1 && 
					    !isnan(d0[qy + hy][qx + hx][0]) && !isnan(D0[hy][hx][0]))
						// use noisy and denoised patches from previous frame
						for (int c  = 0; c  < ch ; ++c )
						{
							const float e1 = n1[qy + hy][qx + hx][c] - N1[hy][hx][c];
							const float e0 = d0[qy + hy][qx + hx][c] - D0[hy][hx][c];
							ww += l * e1 * e1 + (1 - l) * e0 * e0;
						}
					else
						// use only noisy from previous frame
						for (int c  = 0; c  < ch ; ++c )
						{
							const float e1 = n1[qy + hy][qx + hx][c] - N1[hy][hx][c];
							ww += e1 * e1;
						}
	
				// compute similarity weight ]]]4[[[4
				if (weights_hx2)
					ww = expf(-1 / weights_hx2 * ww / (float)(psz*psz));
				else
					ww = (qx == px && qy == py) ? 1. : 0.;
	
				// accumulate on output patch ]]]4[[[4
				if (ww > prms.weights_thx)
				{
					cp += ww;
					for (int hy = 0; hy < psz; ++hy)
					for (int hx = 0; hx < psz; ++hx)
					for (int c  = 0; c  < ch ; ++c )
						D1[hy][hx][c] += n1[qy + hy][qx + hx][c] * ww;
				}// ]]]4
			}
		}
		else
		{
			// copy noisy patch to output
			cp = 1.;
			for (int hy = 0; hy < psz; ++hy)
			for (int hx = 0; hx < psz; ++hx)
			for (int c  = 0; c  < ch ; ++c )
				D1[hy][hx][c] = n1[py + hy][px + hx][c];
		}

		// normalize spatial average [[[3
		float icp = 1. / fmax(cp, 1e-6);
		for (int hy = 0; hy < psz; ++hy)
		for (int hx = 0; hx < psz; ++hx)
		for (int c  = 0; c  < ch ; ++c )
			D1[hy][hx][c] *= icp;

#ifdef VARIANCES
		// TODO: it is unnecessary to have a patch V1 of variances
		// variance of D1
		float vp = sigma * sigma / cp;
		for (int hy = 0; hy < psz; ++hy)
		for (int hx = 0; hx < psz; ++hx)
			V1[hy][hx] = vp;
#endif

		// temporal average with denoised patch at t-1 [[[3
		if (d0)
		{
			// estimate transition error
			float iv = 0., n = 0.;
			for (int hy = 0; hy < psz; ++hy)
			for (int hx = 0; hx < psz; ++hx)
			if (!isnan(D0[hy][hx][0]))
			{
				n ++;
				for (int c  = 0; c  < ch ; ++c )
				{
					const float d = D1[hy][hx][c] - D0[hy][hx][c];
					iv += d * d;
				}
			}
			// normalize and invert
			iv = n*(float)ch / iv * weights_ht2;

			// temporal average
			for (int hy = 0; hy < psz; ++hy)
			for (int hx = 0; hx < psz; ++hx)
			if (!isnan(D0[hy][hx][0]))
			{
				const float v = iv / (iv + icp);
				for (int c  = 0; c  < ch ; ++c )
					D1[hy][hx][c] = (1 - v) * D1[hy][hx][c] + v * D0[hy][hx][c];
			}
		}

		// aggregate denoised patch on output image [[[3
		if (a1)
			// patch-wise denoising: aggregate the whole denoised patch
			for (int hy = 0; hy < psz; ++hy)
			for (int hx = 0; hx < psz; ++hx)
			{
#ifdef VARIANCES
				// weighted aggregation by variance
	#ifdef WEIGHTED_AGGREGATION
				const float w = 1.f/V1[hy][hx];
	#else
				const float w = 1.f;
	#endif
				a1[py + hy][px + hx] += w;
				for (int c = 0; c < ch ; ++c )
					d1[py + hy][px + hx][c] += D1[hy][hx][c] * w;
				v1[py + hy][px + hx] += w * w * V1[hy][hx];
#else
				a1[py + hy][px + hx]++;
				for (int c = 0; c < ch ; ++c )
					d1[py + hy][px + hx][c] += D1[hy][hx][c];
#endif
			}
		else 
		{
#ifdef VARIANCES
			v1[py + psz/2][px + psz/2] = vp * vp;
#endif
			// pixel-wise denoising: aggregate only the central pixel
			for (int c = 0; c < ch ; ++c )
				d1[py + psz/2][px + psz/2][c] += D1[psz/2][psz/2][c];
		}

		// ]]]3
	}

	// normalize output [[[2
	if (aggr1)
	for (int i = 0, j = 0; i < w*h; ++i) 
	for (int c = 0; c < ch ; ++c, ++j) 
		deno1[j] /= aggr1[i];

#ifdef VARIANCES
	if (aggr1)
	for (int i = 0; i < w*h; ++i) 
		vari1[i] /= aggr1[i] * aggr1[i];
#endif

	// free allocated mem and quit
	if (aggr1) free(aggr1);
	return; // ]]]2
}

// main funcion [[[1

// 'usage' message in the command line
static const char *const usages[] = {
	"vnlmeans [options] [[--] args]",
	"vnlmeans [options]",
	NULL,
};

int main(int argc, const char *argv[])
{
	// parse command line [[[2

	// command line parameters and their defaults
	const char *nisy_path = NULL;
	const char *deno_path = NULL;
	const char *flow_path = NULL;
	int fframe = 0, lframe = -1;
	float sigma = 0.f;
	bool verbose = false;
	struct vnlmeans_params prms;
	prms.patch_sz     = -1;
	prms.search_sz    = -1;
	prms.weights_hx   = -1.; // -1 means automatic value
	prms.weights_hd   = -1.;
	prms.weights_thx  = -1.;
	prms.weights_ht   = -1.;
	prms.weights_htv  = -1.;
	prms.dista_lambda = -1.;
	prms.tv_lambda    = -1.;
	prms.pixelwise = false;

	// configure command line parser
	struct argparse_option options[] = {
		OPT_HELP(),
		OPT_GROUP("Algorithm options"),
		OPT_STRING ('i', "nisy"  , &nisy_path, "noisy input path (printf format)"),
		OPT_STRING ('o', "flow"  , &flow_path, "backward flow path (printf format)"),
		OPT_STRING ('d', "deno"  , &deno_path, "denoised output path (printf format)"),
		OPT_INTEGER('f', "first" , &fframe, "first frame"),
		OPT_INTEGER('l', "last"  , &lframe , "last frame"),
		OPT_FLOAT  ('s', "sigma" , &sigma, "noise standard dev"),
		OPT_INTEGER('p', "patch" , &prms.patch_sz, "patch size"),
		OPT_INTEGER('w', "search", &prms.search_sz, "search region radius"),
		OPT_FLOAT  ( 0 , "whx"   , &prms.weights_hx, "spatial patch sim. weights param"),
		OPT_FLOAT  ( 0 , "whd"   , &prms.weights_hd, "spatial distance weights param"),
		OPT_FLOAT  ( 0 , "wthx"  , &prms.weights_thx, "spatial weights threshold"),
		OPT_FLOAT  ( 0 , "wht"   , &prms.weights_ht, "temporal patch sim. weights param"),
		OPT_FLOAT  ( 0 , "whtv"  , &prms.weights_htv, "transition variance weights param"),
		OPT_FLOAT  ( 0 , "lambda", &prms.dista_lambda, "noisy patch weight in patch distance"),
		OPT_FLOAT  ( 0 , "lambtv", &prms.tv_lambda, "noisy patch weight in transition variance"),
		OPT_BOOLEAN( 0 , "pixel" , &prms.pixelwise, "toggle pixel-wise denoising"),
		OPT_GROUP("Program options"),
		OPT_BOOLEAN('v', "verbose", &verbose, "verbose output"),
		OPT_END(),
	};

	// parse command line
	struct argparse argparse;
	argparse_init(&argparse, options, usages, 0);
	argparse_describe(&argparse, "\nA video denoiser based on non-local means.", "");
	argc = argparse_parse(&argparse, argc, argv);

	// default value for noise-dependent params
	vnlmeans_default_params(&prms, sigma);

	// print parameters
	if (verbose)
	{
		printf("parameters:\n");
		printf("\tnoise  %f\n", sigma);
		printf("\t%s-wise mode\n", prms.pixelwise ? "pixel" : "patch");
		printf("\tpatch     %d\n", prms.patch_sz);
		printf("\tsearch    %d\n", prms.search_sz);
		printf("\tw_hx      %g\n", prms.weights_hx);
		printf("\tw_hd      %g\n", prms.weights_hd);
		printf("\tw_thx     %g\n", prms.weights_thx);
		printf("\tw_ht      %g\n", prms.weights_ht);
		printf("\tw_htv     %g\n", prms.weights_htv);
		printf("\tlambda    %g\n", prms.dista_lambda);
		printf("\ttv_lambda %g\n", prms.tv_lambda);
		printf("\n");
#ifdef VARIANCES
		printf("\tVARIANCES ON\n");
#endif
#ifdef WEIGHTED_AGGREGATION
		printf("\tWEIGHTED_AGGREGATION ON\n");
#endif
#ifdef AGGREGATE_TRANSITION_VAR
		printf("\tAGGREGATE_TRANSITION_VAR ON\n");
#endif
	}

	// load data [[[2
	if (verbose) printf("loading video %s\n", nisy_path);
	int w, h, c; //, frames = lframe - fframe + 1;
	float * nisy = vio_read_video_float_vec(nisy_path, fframe, lframe, &w, &h, &c);
	{
		if (!nisy)
			return EXIT_FAILURE;
	}

	// load optical flow
	float * flow = NULL;
	if (flow_path)
	{
		if (verbose) printf("loading flow %s\n", flow_path);
		int w1, h1, c1;
		flow = vio_read_video_float_vec(flow_path, fframe, lframe, &w1, &h1, &c1);

		if (!flow)
		{
			if (nisy) free(nisy);
			return EXIT_FAILURE;
		}

		if (w*h != w1*h1 || c1 != 2)
		{
			fprintf(stderr, "Video and optical flow size missmatch\n");
			if (nisy) free(nisy);
			if (flow) free(flow);
			return EXIT_FAILURE;
		}
	}

	// run denoiser [[[2
	const int whc = w*h*c, wh2 = w*h*2;
	float * deno = nisy;
	float * warp0 = malloc(whc*sizeof(float));
	float * deno1 = malloc(whc*sizeof(float));
#ifdef VARIANCES
	float * vari0 = malloc(w*h*sizeof(float));
	float * vari1 = malloc(w*h*sizeof(float));
#endif
	for (int f = fframe; f <= lframe; ++f)
	{
		if (verbose) printf("processing frame %d\n", f);

		// TODO compute optical flow if absent

		// warp previous denoised frame
		if (f > fframe)
		{
			float * deno0 = deno + (f - 1 - fframe)*whc;
			if (flow)
			{
				float * flow0 = flow + (f - 0 - fframe)*wh2;
				warp_bicubic_inplace(warp0, deno0, flow0, w, h, c);
#ifdef VARIANCES
				warp_bicubic_inplace(vari0, vari1, flow0, w, h, 1);
#endif
			}
			else
				// copy without warping
				memcpy(warp0, deno0, whc*sizeof(float));
		}

		// run denoising
		float *nisy1 = nisy + (f - fframe)*whc;
		float *deno0 = (f > fframe) ? warp0 : NULL;
#ifndef KALMAN_RECURSION
		vnlmeans_frame(deno1, nisy1, deno0, w, h, c, sigma, prms);
#else
	#ifdef VARIANCES
		vnlmeans_kalman_frame(deno1, nisy1, deno0, vari1, vari0, w, h, c, sigma, prms, f);
	#else
		vnlmeans_kalman_frame(deno1, nisy1, deno0, w, h, c, sigma, prms, f);
	#endif


	#ifdef DUMP_INFO
		// write variances image
		char name[512];
		sprintf(name, "dump/var.%03d.tif", f);
		iio_save_image_float_vec(name, vari1, w, h, 1);
//		for (int i = 0; i < w*h; ++i) warp0[i] = sqrtf(vari1[i]);
//		iio_save_image_float_vec(name, warp0, w, h, 1);
	#endif
#endif
		memcpy(nisy1, deno1, whc*sizeof(float));
	}

	// save output [[[2
	vio_save_video_float_vec(deno_path, deno, fframe, lframe, w, h, c);

	if (deno1) free(deno1);
	if (warp0) free(warp0);
#ifdef VARIANCES
	if (vari0) free(vari0);
	if (vari1) free(vari1);
#endif
	if (nisy) free(nisy);
	if (flow) free(flow);

	return EXIT_SUCCESS; // ]]]2
}

// vim:set foldmethod=marker:
// vim:set foldmarker=[[[,]]]:
