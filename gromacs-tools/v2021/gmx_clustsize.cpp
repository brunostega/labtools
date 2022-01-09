/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2007, The GROMACS development team.
 * Copyright (c) 2013,2014,2015,2016,2017 by the GROMACS development team.
 * Copyright (c) 2018,2019,2020, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
#include "gmxpre.h"

#include <cmath>

#include <algorithm>

#include "gromacs/commandline/filenm.h"
#include "gromacs/commandline/pargs.h"
#include "gromacs/fileio/matio.h"
#include "gromacs/fileio/tpxio.h"
#include "gromacs/fileio/trxio.h"
#include "gromacs/fileio/xvgr.h"
#include "gromacs/gmxana/gmx_ana.h"
#include "gromacs/gmxana/gstat.h"
#include "gromacs/gmxlib/nrnb.h"
#include "gromacs/math/units.h"
#include "gromacs/math/vec.h"
#include "gromacs/pbcutil/pbc.h"
#include "gromacs/topology/index.h"
#include "gromacs/topology/mtop_lookup.h"
#include "gromacs/topology/mtop_util.h"
#include "gromacs/topology/topology.h"
#include "gromacs/trajectory/trajectoryframe.h"
#include "gromacs/utility/arraysize.h"
#include "gromacs/utility/cstringutil.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/futil.h"
#include "gromacs/utility/gmxassert.h"
#include "gromacs/utility/gmxomp.h"
#include "gromacs/utility/smalloc.h"

static void clust_size(const char*             ndx,
                       const char*             trx,
                       const char*             xpm,
                       const char*             xpmw,
                       const char*             ncl,
                       const char*             acl,
                       const char*             mcl,
                       const char*             histo,
                       const char*             histotime,
                       const char*             clustime,
                       const char*             tempf,
                       const char*             mcn,
                       gmx_bool                bMol,
                       gmx_bool                bPBC,
                       const char*             tpr,
                       real                    cut,
                       real                    mol_cut,
                       int                     nskip,
                       int                     nlevels,
                       t_rgb                   rmid,
                       t_rgb                   rhi,
                       int                     ndf,
                       int                     nThreads,
                       const gmx_output_env_t* oenv)
{
    FILE *       fp, *gp, *hp, *tp, *cndx;
    int*         index = nullptr;
    int          nindex, natoms; // number of atoms/molecules, number of atoms
    t_trxstatus* status;
    rvec *       x = nullptr, *v = nullptr, dx;
    t_pbc        pbc;
    gmx_bool     bSame, bTPRwarn = TRUE;
    /* Topology stuff */
    t_trxframe    fr;
    TpxFileHeader tpxh;
    gmx_mtop_t*   mtop    = nullptr;
    PbcType       pbcType = PbcType::Unset;
    int           ii, jj;
    real          temp, tfac;
    /* Cluster size distribution (matrix) */
    real** cs_dist = nullptr;
    int**  cs_map = nullptr;
    real   tf, dx2, cut2, *t_x = nullptr, *t_y, cmid, cmax, cav, ekin;
    int    i, j, k, ai, aj, ci, cj, nframe, nclust, n_x, max_size = 0;
    int *  clust_index, *clust_size, *clust_written, max_clust_size, max_clust_ind, nav, nhisto;
    t_rgb  rlo          = { 1.0, 1.0, 1.0 };
    int    frameCounter = 0;
    real   frameTime;
    
    const bool bOMP = GMX_OPENMP;
    int actual_nThreads = 0;
    if(bOMP) {
        actual_nThreads = std::min((nThreads <= 0) ? INT_MAX : nThreads, gmx_omp_get_max_threads());
        gmx_omp_set_num_threads(actual_nThreads);
        printf("Frame loop parallelized with OpenMP using %i threads.\n", actual_nThreads);
        fflush(stdout);
    }
    else
    {
        actual_nThreads = 1;
    }

    clear_trxframe(&fr, TRUE);
    auto timeLabel = output_env_get_time_label(oenv);
    tf             = output_env_get_time_factor(oenv);
    fp             = xvgropen(ncl, "Number of clusters", timeLabel, "N", oenv);
    gp             = xvgropen(acl, "Average cluster size", timeLabel, "#molecules", oenv);
    hp             = xvgropen(mcl, "Max cluster size", timeLabel, "#molecules", oenv);
    tp             = xvgropen(tempf, "Temperature of largest cluster", timeLabel, "T (K)", oenv);

    if (!read_first_frame(oenv, &status, trx, &fr, TRX_NEED_X | TRX_READ_V))
    {
        gmx_file(trx);
    }

    natoms = fr.natoms;
    x      = fr.x;

    if (tpr)
    {
        mtop = new gmx_mtop_t;
        tpxh = readTpxHeader(tpr, true);
        if (tpxh.natoms != natoms)
        {
            gmx_fatal(FARGS, "tpr (%d atoms) and trajectory (%d atoms) do not match!", tpxh.natoms, natoms);
        }
        pbcType = read_tpx(tpr, nullptr, nullptr, &natoms, nullptr, nullptr, mtop);
    }
    if (ndf <= -1)
    {
        tfac = 1;
    }
    else
    {
        tfac = ndf / (3.0 * natoms);
    }

    gmx::RangePartitioning mols;
    if (bMol)
    {
        if (ndx)
        {
            printf("Using molecules rather than atoms. Not reading index file %s\n", ndx);
        }
        GMX_RELEASE_ASSERT(mtop != nullptr, "Trying to access mtop->mols from NULL mtop pointer");
        mols = gmx_mtop_molecules(*mtop);

        /* Make dummy index */
        nindex = mols.numBlocks();
        snew(index, nindex);
        for (i = 0; (i < nindex); i++)
        {
            index[i] = i;
        }
    }
    else
    {
        char* gname;
        rd_index(ndx, 1, &nindex, &index, &gname);
        sfree(gname);
    }

    snew(clust_index, nindex);
    snew(clust_size, nindex);
    // cs_map is used as a contact map to allow to parallelise the following loop in the case of molecules
    snew(cs_map, nindex);
    rvec *xcm;
    snew(xcm, nindex);
    for(i=0;i<nindex;i++) snew(cs_map[i], nindex);
    real mcut2 = mol_cut*mol_cut;
    cut2   = cut * cut;
    // total number of trajectory frames
    nframe = 0;
    // number of analysed frames
    n_x    = 0;
    snew(t_y, nindex);
    for (i = 0; (i < nindex); i++)
    {
        t_y[i] = i + 1;
    }
    max_clust_size = 1;
    max_clust_ind  = -1;
    int molb       = 0;
    cndx = fopen(clustime, "w");
    do
    {
        if ((nskip == 0) || ((nskip > 0) && ((nframe % nskip) == 0)))
        {
            if (bPBC)
            {
                set_pbc(&pbc, pbcType, fr.box);
            }
            max_clust_size = 1;
            max_clust_ind  = -1;

            /* Put all atoms/molecules in their own cluster, with size 1 */
            for (i = 0; (i < nindex); i++)
            {
                /* Cluster index is indexed with atom index number */
                clust_index[i] = i;
                /* Cluster size is indexed with cluster number */
                clust_size[i] = 1;
                clear_rvec(xcm[i]);
            }
            /* calculate the center of each molecule */
            for (i = 0; (i < nindex); i++)
            {   
                ai = index[i];
                real tm = 0.;
                for (ii = mols.block(ai).begin(); ii < mols.block(ai).end(); ii++)
                {
                    for (int m = 0; (m < DIM); m++)
                    {
                        xcm[i][m] += x[ii][m];
                    }
                    tm += 1.0; 
                }
                for (int m = 0; (m < DIM); m++)
                {
                    xcm[i][m] /= tm;
                }
            }

            /* Loop over atoms/molecules */
#pragma omp parallel for 
            for (i = 0; (i < nindex); i++)
            {
                ai = index[i];
                /* Loop over atoms/molecules (only half a matrix) */
                for (j = i + 1; (j < nindex); j++)
                {
                    if (bPBC)
                    {
                        pbc_dx(&pbc, xcm[i], xcm[j], dx);
                    }
                    else
                    {
                        rvec_sub(xcm[i], xcm[j], dx);
                    }
                    dx2   = norm2(dx);
                    if (dx2 > mcut2) continue;
                    bSame = FALSE;
                    aj = index[j];
                    /* Compute distance */
                    for (ii = mols.block(ai).begin(); ii < mols.block(ai).end(); ii++)
                    {
                        for (jj = mols.block(aj).begin(); jj < mols.block(aj).end(); jj++)
                        {
                            if (bPBC)
                            {
                                pbc_dx(&pbc, x[ii], x[jj], dx);
                            }
                            else
                            {
                                rvec_sub(x[ii], x[jj], dx);
                            }
                            dx2   = norm2(dx);
                            bSame = (dx2 < cut2);
                            if(bSame) break;
                        }
                        if (bSame) break;
                    }
                    if (bSame)
                    {
                        cs_map[i][j] = 1;
                    }
                }
            }
            /* This loop is the one to merge the clusters */
            /* Loop over atoms/molecules */
#pragma omp barrier
            for (i = 0; (i < nindex); i++)
            {
                ci = clust_index[i];

                /* Loop over atoms/molecules (only half a matrix) */
                for (j = i + 1; (j < nindex); j++)
                {
                    cj = clust_index[j];

                    /* If they are not in the same cluster already */
                    if (ci != cj)
                    {
                        /* If distance less than cut-off */
                        if (cs_map[i][j])
                        {
                            /* Merge clusters: check for all atoms whether they are in
                             * cluster cj and if so, put them in ci
                             */
                            for (k = 0; (k < nindex); k++)
                            {
                                if (clust_index[k] == cj)
                                {
                                    if (clust_size[cj] <= 0)
                                    {
                                        gmx_fatal(FARGS, "negative cluster size %d for element %d",
                                                  clust_size[cj], cj);
                                    }
                                    clust_size[cj]--;
                                    clust_index[k] = ci;
                                    clust_size[ci]++;
                                }
                            }
                        }
                    }
                }
            }
            n_x++;
            srenew(t_x, n_x);
            if (fr.bTime)
            {
                frameTime = fr.time;
            }
            else if (fr.bStep)
            {
                frameTime = fr.step;
            }
            else
            {
                frameTime = ++frameCounter;
            }
            t_x[n_x - 1] = frameTime * tf;
            srenew(cs_dist, n_x);
            snew(cs_dist[n_x - 1], nindex);
            nclust = 0;
            cav    = 0;
            nav    = 0;
            for (i = 0; (i < nindex); i++)
            {
                ci = clust_size[i];
                if (ci > max_clust_size)
                {
                    max_clust_size = ci;
                    max_clust_ind  = i;
                }
                if (ci > 0)
                {
                    nclust++;
                    cs_dist[n_x - 1][ci - 1] += 1.0;
                    max_size = std::max(max_size, ci);
                    if (ci > 1)
                    {
                        cav += ci;
                        nav++;
                    }
                }
            }
            fprintf(fp, "%14.6e  %10d\n", frameTime, nclust);
            if (nav > 0)
            {
                fprintf(gp, "%14.6e  %10.3f\n", frameTime, cav / nav);
            }
            fprintf(hp, "%14.6e  %10d\n", frameTime, max_clust_size);
        }
        /* Analyse velocities, if present */
        if (fr.bV)
        {
            if (!tpr)
            {
                if (bTPRwarn)
                {
                    printf("You need a [REF].tpr[ref] file to analyse temperatures\n");
                    bTPRwarn = FALSE;
                }
            }
            else
            {
                v = fr.v;
                /* Loop over clusters and for each cluster compute 1/2 m v^2 */
                if (max_clust_ind >= 0)
                {
                    ekin = 0;
                    for (i = 0; (i < nindex); i++)
                    {
                        if (clust_index[i] == max_clust_ind)
                        {
                            ai     = index[i];
                            real m = mtopGetAtomMass(mtop, ai, &molb);
                            ekin += 0.5 * m * iprod(v[ai], v[ai]);
                        }
                    }
                    temp = (ekin * 2.0) / (3.0 * tfac * max_clust_size * BOLTZ);
                    fprintf(tp, "%10.3f  %10.3f\n", frameTime, temp);
                }
            }
        }
        // For each molecule we could write to which cluster it belongs, this may be used to write a chain ID to identify each cluster at a given time
        // but how do you do transition?
        fprintf(cndx, "%10.3f ", frameTime);
        for (i = 0; (i < nindex); i++) fprintf(cndx, "%i ", clust_index[i]);
        fprintf(cndx, "\n");
        nframe++;
    } while (read_next_frame(oenv, status, &fr));
    close_trx(status);
    done_frame(&fr);
    xvgrclose(fp);
    xvgrclose(gp);
    xvgrclose(hp);
    xvgrclose(tp);
    fclose(cndx); 

    snew(clust_written, nindex);
    if (max_clust_ind >= 0)
    {
        fp = gmx_ffopen(mcn, "w");
      /* CARLO: this adds the indices for all the clusters at the end of the trajectory */
      if (bMol)
      {
        for (int i = 0; (i < nindex); i++)
        {
	  ci = clust_index[i];
	  if(clust_written[ci]==1) continue;
          fprintf(fp, "[ clust %i ]\n", ci);
          for (int j : mols.block(i))
          {
             fprintf(fp, "%d\n", j+1);
          }
	  for(int j=i+1; (j < nindex); j++)
	  {
	    if(clust_index[j]==ci) 
	    {
              for (int k : mols.block(j))
              {
                fprintf(fp, "%d\n", k+1);
              }
	    }
	  }
	  clust_written[ci]=1;
	}
      }
        fprintf(fp, "[ max_clust ]\n");
        for (i = 0; (i < nindex); i++)
        {
            if (clust_index[i] == max_clust_ind)
            {
                if (bMol)
                {
                    GMX_RELEASE_ASSERT(mols.numBlocks() > 0,
                                       "Cannot access index[] from empty mols");
                    for (int j : mols.block(i))
                    {
                        fprintf(fp, "%d\n", j + 1);
                    }
                }
                else
                {
                    fprintf(fp, "%d\n", index[i] + 1);
                }
            }
        }
        gmx_ffclose(fp);
    }

    /* Print the real distribution cluster-size/numer, averaged over the trajectory. */
    fp     = xvgropen(histo, "Cluster size distribution", "Cluster size", "()", oenv);
    nhisto = 0;
    fprintf(fp, "%5d  %8.3f\n", 0, 0.0);
    for (j = 0; (j < max_size); j++)
    {
        real nelem = 0;
        for (i = 0; (i < n_x); i++)
        {
            nelem += cs_dist[i][j];
        }
        fprintf(fp, "%5d  %8.3f\n", j + 1, nelem / n_x);
        nhisto += static_cast<int>((j + 1) * nelem / n_x);
    }
    fprintf(fp, "%5d  %8.3f\n", j + 1, 0.0);
    xvgrclose(fp);

    fp = fopen(histotime, "w");
    for (i = 0; (i < n_x); i++)
    {
        fprintf(fp, "%5d ", i);
    	for (j = 0; (j < max_size); j++)
    	{
        	fprintf(fp, " %8.3f", cs_dist[i][j]);
        }
        fprintf(fp,"\n");
    }
    fclose(fp); 

    fprintf(stderr, "Total number of atoms in clusters =  %d\n", nhisto);

    /* Look for the smallest entry that is not zero
     * This will make that zero is white, and not zero is coloured.
     */
    cmid = 100.0;
    cmax = 0.0;
    for (i = 0; (i < n_x); i++)
    {
        for (j = 0; (j < max_size); j++)
        {
            if ((cs_dist[i][j] > 0) && (cs_dist[i][j] < cmid))
            {
                cmid = cs_dist[i][j];
            }
            cmax = std::max(cs_dist[i][j], cmax);
        }
    }
    fprintf(stderr, "cmid: %g, cmax: %g, max_size: %d\n", cmid, cmax, max_size);
    cmid = 1;
    fp   = gmx_ffopen(xpm, "w");
    write_xpm3(fp, 0, "Cluster size distribution", "# clusters", timeLabel, "Size", n_x, max_size,
               t_x, t_y, cs_dist, 0, cmid, cmax, rlo, rmid, rhi, &nlevels);
    gmx_ffclose(fp);
    cmid = 100.0;
    cmax = 0.0;
    for (i = 0; (i < n_x); i++)
    {
        for (j = 0; (j < max_size); j++)
        {
            cs_dist[i][j] *= (j + 1);
            if ((cs_dist[i][j] > 0) && (cs_dist[i][j] < cmid))
            {
                cmid = cs_dist[i][j];
            }
            cmax = std::max(cs_dist[i][j], cmax);
        }
    }
    fprintf(stderr, "cmid: %g, cmax: %g, max_size: %d\n", cmid, cmax, max_size);
    fp = gmx_ffopen(xpmw, "w");
    write_xpm3(fp, 0, "Weighted cluster size distribution", "Fraction", timeLabel, "Size", n_x,
               max_size, t_x, t_y, cs_dist, 0, cmid, cmax, rlo, rmid, rhi, &nlevels);
    gmx_ffclose(fp);
    delete mtop;
    sfree(t_x);
    sfree(t_y);
    for (i = 0; (i < n_x); i++)
    {
        sfree(cs_dist[i]);
    }
    sfree(cs_dist);
    sfree(clust_index);
    sfree(clust_size);
    sfree(index);
}

int gmx_clustsize(int argc, char* argv[])
{
    const char* desc[] = {
        "[THISMODULE] computes the size distributions of molecular/atomic clusters in",
        "the gas phase. The output is given in the form of an [REF].xpm[ref] file.",
        "The total number of clusters is written to an [REF].xvg[ref] file.[PAR]",
        "When the [TT]-mol[tt] option is given clusters will be made out of",
        "molecules rather than atoms, which allows clustering of large molecules.",
        "In this case an index file would still contain atom numbers",
        "or your calculation will die with a SEGV.[PAR]",
        "When velocities are present in your trajectory, the temperature of",
        "the largest cluster will be printed in a separate [REF].xvg[ref] file assuming",
        "that the particles are free to move. If you are using constraints,",
        "please correct the temperature. For instance water simulated with SHAKE",
        "or SETTLE will yield a temperature that is 1.5 times too low. You can",
        "compensate for this with the [TT]-ndf[tt] option. Remember to take the removal",
        "of center of mass motion into account.[PAR]",
        "The [TT]-mc[tt] option will produce an index file containing the",
        "atom numbers of the largest cluster."
    };

    static int      nThreads = 0;

    real     cutoff  = 0.35;
    real     mol_cutoff  = 2.00;
    int      nskip   = 0;
    int      nlevels = 20;
    int      ndf     = -1;
    gmx_bool bMol    = FALSE;
    gmx_bool bPBC    = TRUE;
    rvec     rlo     = { 1.0, 1.0, 0.0 };
    rvec     rhi     = { 0.0, 0.0, 1.0 };

    gmx_output_env_t* oenv;

    t_pargs pa[] = {
        { "-cut",
          FALSE,
          etREAL,
          { &cutoff },
          "Largest distance (nm) to be considered in a cluster" },
        { "-mol_cut",
          FALSE,
          etREAL,
          { &mol_cutoff },
          "Largest distance (nm) to be considered between molecules in a cluster" },
        { "-mol",
          FALSE,
          etBOOL,
          { &bMol },
          "Cluster molecules rather than atoms (needs [REF].tpr[ref] file)" },
        { "-pbc", FALSE, etBOOL, { &bPBC }, "Use periodic boundary conditions" },
        { "-nskip", FALSE, etINT, { &nskip }, "Number of frames to skip between writing" },
        { "-nlevels",
          FALSE,
          etINT,
          { &nlevels },
          "Number of levels of grey in [REF].xpm[ref] output" },
        { "-ndf",
          FALSE,
          etINT,
          { &ndf },
          "Number of degrees of freedom of the entire system for temperature calculation. "
          "If not set, the number of atoms times three is used." },
        { "-rgblo",
          FALSE,
          etRVEC,
          { rlo },
          "RGB values for the color of the lowest occupied cluster size" },
        { "-rgbhi",
          FALSE,
          etRVEC,
          { rhi },
          "RGB values for the color of the highest occupied cluster size" },
#if GMX_OPENMP
        { "-nthreads",
          FALSE,
          etINT,
          { &nThreads },
          "Number of threads used for the parallel loop over autocorrelations. nThreads <= 0 means "
          "maximum number of threads. Requires linking with OpenMP. The number of threads is "
          "limited by the number of cores (before OpenMP v.3 ) or environment variable "
          "OMP_THREAD_LIMIT (OpenMP v.3)" }
#endif
    };
#define NPA asize(pa)
    const char *fnNDX, *fnTPR;
    t_rgb       rgblo, rgbhi;

    t_filenm fnm[] = {
        { efTRX, "-f", nullptr, ffREAD },         { efTPR, nullptr, nullptr, ffOPTRD },
        { efNDX, nullptr, nullptr, ffOPTRD },     { efXPM, "-o", "csize", ffWRITE },
        { efXPM, "-ow", "csizew", ffWRITE },      { efXVG, "-nc", "nclust", ffWRITE },
        { efXVG, "-mc", "maxclust", ffWRITE },    { efXVG, "-ac", "avclust", ffWRITE },
        { efXVG, "-hc", "histo-clust", ffWRITE }, { efXVG, "-temp", "temp", ffOPTWR },
        { efDAT, "-hct", "histo-time", ffWRITE },
        { efDAT, "-ict", "clust-index-time", ffWRITE },
        { efNDX, "-mcn", "maxclust", ffOPTWR }
    };
#define NFILE asize(fnm)

    if (!parse_common_args(&argc, argv, PCA_CAN_VIEW | PCA_CAN_TIME | PCA_TIME_UNIT, NFILE, fnm,
                           NPA, pa, asize(desc), desc, 0, nullptr, &oenv))
    {
        return 0;
    }

    fnNDX   = ftp2fn_null(efNDX, NFILE, fnm);
    rgblo.r = rlo[XX];
    rgblo.g = rlo[YY];
    rgblo.b = rlo[ZZ];
    rgbhi.r = rhi[XX];
    rgbhi.g = rhi[YY];
    rgbhi.b = rhi[ZZ];

    fnTPR = ftp2fn_null(efTPR, NFILE, fnm);
    if (bMol && !fnTPR)
    {
        gmx_fatal(FARGS, "You need a tpr file for the -mol option");
    }

    clust_size(fnNDX, ftp2fn(efTRX, NFILE, fnm), opt2fn("-o", NFILE, fnm), opt2fn("-ow", NFILE, fnm),
               opt2fn("-nc", NFILE, fnm), opt2fn("-ac", NFILE, fnm), opt2fn("-mc", NFILE, fnm),
               opt2fn("-hc", NFILE, fnm), opt2fn("-hct", NFILE, fnm), opt2fn("-ict", NFILE, fnm), opt2fn("-temp", NFILE, fnm), opt2fn("-mcn", NFILE, fnm),
               bMol, bPBC, fnTPR, cutoff, mol_cutoff, nskip, nlevels, rgblo, rgbhi, ndf, nThreads, oenv);

    output_env_done(oenv);

    return 0;
}
