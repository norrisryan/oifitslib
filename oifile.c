/**
 * @file
 * @ingroup oifile
 * Implementation of file-level API for OIFITS data.
 *
 * Copyright (C) 2007, 2015 John Young
 *
 *
 * This file is part of OIFITSlib.
 *
 * OIFITSlib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * OIFITSlib is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with OIFITSlib.  If not, see
 * http://www.gnu.org/licenses/
 */

#include "oifile.h"
#include "datemjd.h"

#include <stdlib.h>
#include <string.h>

#include <fitsio.h>


/** GLib expanding string buffer, for use within OIFITSlib. */
GString *pGStr = NULL;


/** Typedef to specify pointer to a function that frees its argument. */
typedef void (*free_func)(gpointer);


/*
 * Private functions
 */

/** Find oi_array matching arrname in linked list. */
static oi_array *find_oi_array(const oi_fits *pOi,
			       const char *arrname)
{
  GList *link;
  oi_array *pArray;

  link = pOi->arrayList;
  while(link != NULL) {
    pArray = (oi_array *) link->data;
    if(strcmp(pArray->arrname, arrname) == 0)
      return pArray;
    link = link->next;
  }
  g_warning("Missing OI_ARRAY with ARRNAME=%s", arrname);
  return NULL;
}

/** Find oi_wavelength matching insname in linked list. */
static oi_wavelength *find_oi_wavelength(const oi_fits *pOi,
                                         const char *insname)
{
  GList *link;
  oi_wavelength *pWave;

  link = pOi->wavelengthList;
  while(link != NULL) {
    pWave = (oi_wavelength *) link->data;
    if(strcmp(pWave->insname, insname) == 0)
      return pWave;
    link = link->next;
  }
  g_warning("Missing OI_WAVELENGTH with INSNAME=%s", insname);
  return NULL;
}

/** Find oi_corr matching corrname in linked list. */
static oi_corr *find_oi_corr(const oi_fits *pOi, const char *corrname)
{
  GList *link;
  oi_corr *pCorr;

  link = pOi->corrList;
  while(link != NULL) {
    pCorr = (oi_corr *) link->data;
    if(strcmp(pCorr->corrname, corrname) == 0)
      return pCorr;
    link = link->next;
  }
  g_warning("Missing OI_CORR with CORRNAME=%s", corrname);
  return NULL;
}


/**
 * Return shortest wavelength in OI_WAVELENGTH table.
 *
 *   @param pWave  pointer to wavelength struct
 *
 *   @return Minimum of eff_wave values /m
 */
static float get_min_wavelength(const oi_wavelength *pWave)
{
  int i;
  float minWave = 1.0e11;

  for(i=0; i<pWave->nwave; i++) {
    if (pWave->eff_wave[i] < minWave) minWave = pWave->eff_wave[i];
  }
  return minWave;
}

/**
 * Return longest wavelength in OI_WAVELENGTH table.
 *
 *   @param pWave  pointer to wavelength struct
 *
 *   @return Maximum of eff_wave values /m
 */
static float get_max_wavelength(const oi_wavelength *pWave)
{
  int i;
  float maxWave = 0.0;

  for(i=0; i<pWave->nwave; i++) {
    if (pWave->eff_wave[i] > maxWave) maxWave = pWave->eff_wave[i];
  }
  return maxWave;
}

/** Generate summary string for each oi_array in GList. */
static void format_array_list_summary(GString *pGStr, GList *arrayList)
{
  int nn;
  GList *link;
  oi_array *pArray;

  nn = 1;
  link = arrayList;
  while(link != NULL) {
    pArray = (oi_array *) link->data;
    g_string_append_printf(pGStr,
			   "    #%-2d ARRNAME='%s'  %d elements\n",
			   nn++, pArray->arrname, pArray->nelement);
    link = link->next;
  }
}    

/** Generate summary string for each oi_wavelength in GList. */
static void format_wavelength_list_summary(GString *pGStr, GList *waveList)
{
  int nn;
  GList *link;
  oi_wavelength *pWave;

  nn = 1;
  link = waveList;
  while(link != NULL) {
    pWave = (oi_wavelength *) link->data;
    g_string_append_printf(pGStr,
                           "    #%-2d INSNAME='%s'  %d channels  "
                           "%7.1f-%7.1fnm\n",
			   nn++, pWave->insname, pWave->nwave,
			   1e9*get_min_wavelength(pWave),
			   1e9*get_max_wavelength(pWave));
    link = link->next;
  }
}    

/** Generate summary string for each oi_corr in GList. */
static void format_corr_list_summary(GString *pGStr, GList *corrList)
{
  int nn;
  GList *link;
  oi_corr *pCorr;

  nn = 1;
  link = corrList;
  while(link != NULL) {
    pCorr = (oi_corr *) link->data;
    g_string_append_printf(pGStr,
                           "    #%-2d CORRNAME='%s'  "
                           "%d/%d non-zero correlations\n",
			   nn++, pCorr->corrname, pCorr->ncorr, pCorr->ndata);
    link = link->next;
  }
}    

/** Generate summary string for each oi_polar in GList. */
static void format_polar_list_summary(GString *pGStr, GList *polarList)
{
  int nn;
  GList *link;
  oi_polar *pPolar;

  nn = 1;
  link = polarList;
  while(link != NULL) {
    pPolar = (oi_polar *) link->data;
    //:TODO: add list of unique INSNAME values in this table
    g_string_append_printf(pGStr,
			   "    #%-2d ARRNAME='%s'\n", nn++, pPolar->arrname);
    link = link->next;
  }
}    

/**
 * Return earliest of binary table DATE-OBS values as MJD.
 */
static long min_mjd(const oi_fits *pOi)
{
  const GList *link;
  oi_vis *pVis;
  oi_vis2 *pVis2;
  oi_t3 *pT3;
  oi_spectrum *pSpectrum;
  long year, month, day, mjd, minMjd;

  minMjd = 100000;
  link = pOi->visList;
  while (link != NULL) {
    pVis = link->data;
    if (sscanf(pVis->date_obs, "%4ld-%2ld-%2ld", &year, &month, &day) == 3
        && (mjd = date2mjd(year, month, day)) < minMjd)
      minMjd = mjd;
    link = link->next;
  }
  link = pOi->vis2List;
  while (link != NULL) {
    pVis2 = link->data;
    if (sscanf(pVis2->date_obs, "%4ld-%2ld-%2ld", &year, &month, &day) == 3
        && (mjd = date2mjd(year, month, day)) < minMjd)
      minMjd = mjd;
    link = link->next;
  }
  link = pOi->t3List;
  while (link != NULL) {
    pT3 = link->data;
    if (sscanf(pT3->date_obs, "%4ld-%2ld-%2ld", &year, &month, &day) == 3
        && (mjd = date2mjd(year, month, day)) < minMjd)
      minMjd = mjd;
    link = link->next;
  }
  link = pOi->spectrumList;
  while (link != NULL) {
    pSpectrum = link->data;
    if (sscanf(pSpectrum->date_obs, "%4ld-%2ld-%2ld", &year, &month, &day) == 3
        && (mjd = date2mjd(year, month, day)) < minMjd)
      minMjd = mjd;
    link = link->next;
  }
  return minMjd;
}


/*
 * Public functions
 */

/**
 * Initialise empty oi_fits struct.
 *
 *   @param pOi  pointer to file data struct, see oifile.h
 */
void init_oi_fits(oi_fits *pOi)
{
  pOi->header.origin[0] = '\0';
  pOi->header.date_obs[0] = '\0';
  pOi->header.telescop[0] = '\0';
  pOi->header.instrume[0] = '\0';
  pOi->header.insmode[0] = '\0';
  pOi->header.object[0] = '\0';
  pOi->header.referenc[0] = '\0';
  pOi->header.prog_id[0] = '\0';
  pOi->header.procsoft[0] = '\0';
  pOi->header.obstech[0] = '\0';

  pOi->targets.revision = 2;
  pOi->targets.ntarget = 0;
  pOi->targets.targ = NULL;
  pOi->targets.usecategory = FALSE;

  pOi->numArray = 0;
  pOi->numWavelength = 0;
  pOi->numCorr = 0;
  pOi->numPolar = 0;
  pOi->numVis = 0;
  pOi->numVis2 = 0;
  pOi->numT3 = 0;
  pOi->numSpectrum = 0;
  pOi->arrayList = NULL;
  pOi->wavelengthList = NULL;
  pOi->corrList = NULL;
  pOi->polarList = NULL;
  pOi->visList = NULL;
  pOi->vis2List = NULL;
  pOi->t3List = NULL;
  pOi->spectrumList = NULL;
  pOi->arrayHash = 
    g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
  pOi->wavelengthHash =
    g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
  pOi->corrHash =
    g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
}


#define RETURN_VAL_IF_BAD_TAB_REVISION(tabList, tabType, rev, val) \
  {                                                                     \
    tabType *tab;                                                       \
    GList *link;                                                        \
    link = (tabList);                                                   \
    while (link != NULL) {                                              \
      tab = (tabType *) link->data;                                     \
      if (tab->revision != rev) return (val);                           \
      link = link->next;                                                \
    }                                                                   \
  }

/**
 * Do all table revision numbers match version 1 of the OIFITS standard?
 *
 * Ignores any tables defined in OIFITS version 2.
 *
 *   @param pOi  pointer to file data struct, see oifile.h
 *
 *   @return TRUE if OIFITS v1, FALSE otherwise.
 */
int is_oi_fits_one(const oi_fits *pOi)
{
  g_assert(pOi != NULL);
  if (pOi->targets.revision != 1) return FALSE;
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->arrayList, oi_array, 1, FALSE);
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->wavelengthList, oi_wavelength, 1, FALSE);
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->visList, oi_vis, 1, FALSE);
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->vis2List, oi_vis2, 1, FALSE);
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->t3List, oi_t3, 1, FALSE);
  return TRUE;
}

/**
 * Do all table revision numbers match version 2 of the OIFITS standard?
 *
 *   @param pOi  pointer to file data struct, see oifile.h
 *
 *   @return TRUE if OIFITS v2, FALSE otherwise.
 */
int is_oi_fits_two(const oi_fits *pOi)
{
  g_assert(pOi != NULL);
  //:TODO: also check PRODCATG from primary header?
  if (pOi->targets.revision != 2) return FALSE;
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->arrayList, oi_array, 2, FALSE);
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->wavelengthList, oi_wavelength, 2, FALSE);
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->corrList, oi_corr, 1, FALSE);
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->polarList, oi_polar, 1, FALSE);
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->visList, oi_vis, 2, FALSE);
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->vis2List, oi_vis2, 2, FALSE);
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->t3List, oi_t3, 2, FALSE);
  RETURN_VAL_IF_BAD_TAB_REVISION(pOi->spectrumList, oi_spectrum, 1, FALSE);
  return TRUE;
}


/** Macro to write FITS table for each oi_* in GList. */
#define WRITE_OI_LIST(fptr, list, type, link, write_func, \
                      extver, pStatus) \
  link = list; \
  extver = 1; \
  while(link != NULL) { \
    write_func(fptr, *((type *) link->data), extver++, pStatus); \
    link = link->next; \
  }

/**
 * Set primary header keywords from table contents.
 *
 * Sets values for DATE-OBS, TELESCOP, INSTRUME and OBJECT from
 * existing data.  Note that the mandatory keywords ORIGIN and INSMODE
 * are not set.
 *
 *   @param pOi  pointer to file data struct, see oifile.h
 */
void set_oi_header(oi_fits *pOi)
{
  const char multiple[] = "MULTIPLE";
  oi_array *pArray;
  oi_wavelength *pWave;
  long year, month, day;

  /* Set TELESCOP */
  if (pOi->numArray == 0) {
    g_strlcpy(pOi->header.telescop, "UNKNOWN", FLEN_VALUE);
  } else if (pOi->numArray == 1) {
    pArray = pOi->arrayList->data;
    g_strlcpy(pOi->header.telescop, pArray->arrname, FLEN_VALUE);
  } else {
    g_strlcpy(pOi->header.telescop, multiple, FLEN_VALUE);
  }

  /* Set INSTRUME */
  if (pOi->numWavelength == 1) {
    pWave = pOi->wavelengthList->data;
    g_strlcpy(pOi->header.instrume, pWave->insname, FLEN_VALUE);
  } else {
    g_strlcpy(pOi->header.instrume, multiple, FLEN_VALUE);
  }

  /* Set OBJECT */
  if (pOi->targets.ntarget == 1) {
    g_strlcpy(pOi->header.object, pOi->targets.targ[0].target, FLEN_VALUE);
  } else {
    g_strlcpy(pOi->header.object, multiple, FLEN_VALUE);
  }

  /* Set DATE-OBS */
  mjd2date(min_mjd(pOi), &year, &month, &day);
  g_snprintf(pOi->header.date_obs, FLEN_VALUE,
             "%4ld-%02ld-%02ld", year, month, day);
}

/**
 * Write OIFITS tables to new FITS file.
 *
 *   @param filename  name of file to create
 *   @param oi        file data struct, see oifile.h
 *   @param pStatus   pointer to status variable
 *
 *   @return On error, returns non-zero cfitsio error code (also assigned to
 *           *pStatus)
 */
STATUS write_oi_fits(const char *filename, oi_fits oi, STATUS *pStatus)
{
  const char function[] = "write_oi_fits";
  fitsfile *fptr;
  GList *link;
  int extver;

  if (*pStatus) return *pStatus; /* error flag set - do nothing */

  /* Open new FITS file */
  fits_create_file(&fptr, filename, pStatus);
  if(*pStatus) goto except;

  /* Write primary header keywords */
  write_oi_header(fptr, oi.header, pStatus);

  /* Write OI_TARGET table */
  write_oi_target(fptr, oi.targets, pStatus);

  /* Write all OI_ARRAY tables */
  WRITE_OI_LIST(fptr, oi.arrayList, oi_array, link,
		write_oi_array, extver, pStatus);

  /* Write all OI_WAVELENGTH tables */
  WRITE_OI_LIST(fptr, oi.wavelengthList, oi_wavelength, link,
		write_oi_wavelength, extver, pStatus);

  /* Write all OI_CORR tables */
  WRITE_OI_LIST(fptr, oi.corrList, oi_corr, link,
                write_oi_corr, extver, pStatus);

  /* Write all OI_POLAR tables */
  WRITE_OI_LIST(fptr, oi.polarList, oi_polar, link,
                write_oi_polar, extver, pStatus);

  /* Write all data tables */
  WRITE_OI_LIST(fptr, oi.visList, oi_vis, link,
		write_oi_vis, extver, pStatus);
  WRITE_OI_LIST(fptr, oi.vis2List, oi_vis2, link,
		write_oi_vis2, extver, pStatus);
  WRITE_OI_LIST(fptr, oi.t3List, oi_t3, link,
		write_oi_t3, extver, pStatus);
  WRITE_OI_LIST(fptr, oi.spectrumList, oi_spectrum, link,
                write_oi_spectrum, extver, pStatus);
  
  fits_close_file(fptr, pStatus);

 except:
  if (*pStatus && !oi_hush_errors) {
    fprintf(stderr, "CFITSIO error in %s:\n", function);
    fits_report_error(stderr, *pStatus);
  }
  return *pStatus;
}

/**
 * Read all OIFITS tables from FITS file.
 *
 *   @param filename  name of file to read
 *   @param pOi       pointer to uninitialised file data struct, see oifile.h
 *   @param pStatus   pointer to status variable
 *
 *   @return On error, returns non-zero cfitsio error code (also assigned to
 *           *pStatus). Contents of file data struct are undefined
 */
STATUS read_oi_fits(const char *filename, oi_fits *pOi, STATUS *pStatus)
{
  const char function[] = "read_oi_fits";
  fitsfile *fptr;
  int hdutype;
  oi_array *pArray;
  oi_wavelength *pWave;
  oi_corr *pCorr;
  oi_polar *pPolar;
  oi_vis *pVis;
  oi_vis2 *pVis2;
  oi_t3 *pT3;
  oi_spectrum *pSpectrum;

  if (*pStatus) return *pStatus; /* error flag set - do nothing */

  fits_open_file(&fptr, filename, READONLY, pStatus);
  if (*pStatus) goto except;

  /* Create empty data structures */
  pOi->arrayHash = 
    g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
  pOi->wavelengthHash =
    g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
  pOi->corrHash =
    g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
  pOi->arrayList = NULL;
  pOi->wavelengthList = NULL;
  pOi->corrList = NULL;
  pOi->polarList = NULL;
  pOi->visList = NULL;
  pOi->vis2List = NULL;
  pOi->t3List = NULL;
  pOi->spectrumList = NULL;

  /* Read primary header keywords */
  read_oi_header(fptr, &pOi->header, pStatus);

  /* Read compulsory OI_TARGET table */
  read_oi_target(fptr, &pOi->targets, pStatus);
  if (*pStatus) goto except;

  /* Read all OI_ARRAY tables */
  pOi->numArray = 0;
  fits_movabs_hdu(fptr, 1, &hdutype, pStatus); /* back to start */
  while (1==1) {
    pArray = malloc(sizeof(oi_array));
    if (read_next_oi_array(fptr, pArray, pStatus))
      break; /* no more OI_ARRAY */
    pOi->arrayList = g_list_append(pOi->arrayList, pArray);
    ++pOi->numArray;
  }
  free(pArray);
  if(*pStatus != END_OF_FILE) goto except;
  *pStatus = 0; /* reset EOF */

  /* Read all OI_WAVELENGTH tables */
  pOi->numWavelength = 0;
  fits_movabs_hdu(fptr, 1, &hdutype, pStatus); /* back to start */
  while (1==1) {
    pWave = malloc(sizeof(oi_wavelength));
    if (read_next_oi_wavelength(fptr, pWave, pStatus))
      break; /* no more OI_WAVELENGTH */
    pOi->wavelengthList = g_list_append(pOi->wavelengthList, pWave);
    ++pOi->numWavelength;
  }
  free(pWave);
  if(*pStatus != END_OF_FILE) goto except;
  *pStatus = 0; /* reset EOF */

  /* Read all OI_CORR tables */
  pOi->numCorr = 0;
  fits_movabs_hdu(fptr, 1, &hdutype, pStatus); /* back to start */
  while (1==1) {
    pCorr = malloc(sizeof(oi_corr));
    if (read_next_oi_corr(fptr, pCorr, pStatus))
      break; /* no more OI_CORR */
    pOi->corrList = g_list_append(pOi->corrList, pCorr);
    ++pOi->numCorr;
  }
  free(pCorr);
  if(*pStatus != END_OF_FILE) goto except;
  *pStatus = 0; /* reset EOF */

  /* Read all OI_POLAR tables */
  pOi->numPolar = 0;
  fits_movabs_hdu(fptr, 1, &hdutype, pStatus); /* back to start */
  while (1==1) {
    pPolar = malloc(sizeof(oi_polar));
    if (read_next_oi_polar(fptr, pPolar, pStatus))
      break; /* no more OI_POLAR */
    pOi->polarList = g_list_append(pOi->polarList, pPolar);
    ++pOi->numPolar;
  }
  free(pPolar);
  if(*pStatus != END_OF_FILE) goto except;
  *pStatus = 0; /* reset EOF */

  /* Read all OI_VIS, hash-tabling corresponding array, wavelength and
   * corr tables */
  pOi->numVis = 0;
  fits_movabs_hdu(fptr, 1, &hdutype, pStatus); /* back to start */
  while (1==1) {
    pVis = malloc(sizeof(oi_vis));
    if (read_next_oi_vis(fptr, pVis, pStatus)) break; /* no more OI_VIS */
    pOi->visList = g_list_append(pOi->visList, pVis);
    ++pOi->numVis;
    if (strlen(pVis->arrname) > 0) {
      if(!g_hash_table_lookup(pOi->arrayHash, pVis->arrname))
	g_hash_table_insert(pOi->arrayHash, pVis->arrname,
			    find_oi_array(pOi, pVis->arrname));
    }
    if(!g_hash_table_lookup(pOi->wavelengthHash, pVis->insname))
      g_hash_table_insert(pOi->wavelengthHash, pVis->insname,
			  find_oi_wavelength(pOi, pVis->insname));
    if (strlen(pVis->corrname) > 0) {
      if(!g_hash_table_lookup(pOi->corrHash, pVis->corrname))
        g_hash_table_insert(pOi->corrHash, pVis->corrname,
        		       find_oi_corr(pOi, pVis->corrname));
    }
  }
  free(pVis);
  if(*pStatus != END_OF_FILE) goto except;
  *pStatus = 0; /* reset EOF */

  /* Read all OI_VIS2, hash-tabling corresponding array, wavelength
   * and corr tables */
  pOi->numVis2 = 0;
  fits_movabs_hdu(fptr, 1, &hdutype, pStatus); /* back to start */
  while (1==1) {
    pVis2 = malloc(sizeof(oi_vis2));
    if (read_next_oi_vis2(fptr, pVis2, pStatus)) break; /* no more OI_VIS2 */
    pOi->vis2List = g_list_append(pOi->vis2List, pVis2);
    ++pOi->numVis2;
    if (strlen(pVis2->arrname) > 0) {
      if(!g_hash_table_lookup(pOi->arrayHash, pVis2->arrname))
	g_hash_table_insert(pOi->arrayHash, pVis2->arrname,
			    find_oi_array(pOi, pVis2->arrname));
    }
    if(!g_hash_table_lookup(pOi->wavelengthHash, pVis2->insname))
      g_hash_table_insert(pOi->wavelengthHash, pVis2->insname,
			  find_oi_wavelength(pOi, pVis2->insname));
    if (strlen(pVis2->corrname) > 0) {
      if(!g_hash_table_lookup(pOi->corrHash, pVis2->corrname))
        g_hash_table_insert(pOi->corrHash, pVis2->corrname,
        		       find_oi_corr(pOi, pVis2->corrname));
    }
  }
  free(pVis2);
  if(*pStatus != END_OF_FILE) goto except;
  *pStatus = 0; /* reset EOF */

  /* Read all OI_T3, hash-tabling corresponding array, wavelength and
   * corr tables */
  pOi->numT3 = 0;
  fits_movabs_hdu(fptr, 1, &hdutype, pStatus); /* back to start */
  while (1==1) {
    pT3 = malloc(sizeof(oi_t3));
    if (read_next_oi_t3(fptr, pT3, pStatus)) break; /* no more OI_T3 */
    pOi->t3List = g_list_append(pOi->t3List, pT3);
    ++pOi->numT3;
    if (strlen(pT3->arrname) > 0) {
      if(!g_hash_table_lookup(pOi->arrayHash, pT3->arrname))
	g_hash_table_insert(pOi->arrayHash, pT3->arrname,
			    find_oi_array(pOi, pT3->arrname));
    }
    if(!g_hash_table_lookup(pOi->wavelengthHash, pT3->insname))
      g_hash_table_insert(pOi->wavelengthHash, pT3->insname,
			  find_oi_wavelength(pOi, pT3->insname));
    if (strlen(pT3->corrname) > 0) {
      if(!g_hash_table_lookup(pOi->corrHash, pT3->corrname))
        g_hash_table_insert(pOi->corrHash, pT3->corrname,
        		       find_oi_corr(pOi, pT3->corrname));
    }
  }
  free(pT3);
  if(*pStatus != END_OF_FILE) goto except;
  *pStatus = 0; /* reset EOF */

  /* Read all OI_SPECTRUM, hash-tabling corresponding array &
   * wavelength tables */
  pOi->numSpectrum = 0;
  fits_movabs_hdu(fptr, 1, &hdutype, pStatus); /* back to start */
  while (1==1) {
    pSpectrum = malloc(sizeof(oi_spectrum));
    if (read_next_oi_spectrum(fptr, pSpectrum, pStatus))
      break; /* no more OI_SPECTRUM */
    pOi->spectrumList = g_list_append(pOi->spectrumList, pSpectrum);
    ++pOi->numSpectrum;
    if (strlen(pSpectrum->arrname) > 0) {
      if(!g_hash_table_lookup(pOi->arrayHash, pSpectrum->arrname))
	g_hash_table_insert(pOi->arrayHash, pSpectrum->arrname,
			    find_oi_array(pOi, pSpectrum->arrname));
    }
    if(!g_hash_table_lookup(pOi->wavelengthHash, pSpectrum->insname))
      g_hash_table_insert(pOi->wavelengthHash, pSpectrum->insname,
			  find_oi_wavelength(pOi, pSpectrum->insname));
  }
  free(pSpectrum);
  if(*pStatus != END_OF_FILE) goto except;
  *pStatus = 0; /* reset EOF */

  if (is_oi_fits_one(pOi))
    set_oi_header(pOi);

  fits_close_file(fptr, pStatus);

 except:
  if (*pStatus && !oi_hush_errors) {
    fprintf(stderr, "CFITSIO error in %s:\n", function);
    fits_report_error(stderr, *pStatus);
  }
  return *pStatus;
}

/** Free linked list and contents. */
static void free_list(GList *list, free_func internalFree)
{
  GList *link;

  link = list;
  while(link != NULL) {
    if(internalFree)
      (*internalFree)(link->data);
    free(link->data);
    link = link->next;
  }
  g_list_free(list);

}

/**
 * Free storage used for OIFITS data.
 *
 *   @param pOi     pointer to file data struct, see oifile.h
 */
void free_oi_fits(oi_fits *pOi)
{
  g_hash_table_destroy(pOi->arrayHash);
  g_hash_table_destroy(pOi->wavelengthHash);
  g_hash_table_destroy(pOi->corrHash);
  free_oi_target(&pOi->targets);
  free_list(pOi->arrayList,
	    (free_func) free_oi_array);
  free_list(pOi->wavelengthList,
	    (free_func) free_oi_wavelength);
  free_list(pOi->corrList,
            (free_func) free_oi_corr);
  free_list(pOi->polarList,
            (free_func) free_oi_polar);
  free_list(pOi->visList,
	    (free_func) free_oi_vis);
  free_list(pOi->vis2List,
	    (free_func) free_oi_vis2);
  free_list(pOi->t3List,
	    (free_func) free_oi_t3);
  free_list(pOi->spectrumList,
            (free_func) free_oi_spectrum);
}

/**
 * Return oi_array corresponding to specified ARRNAME.
 *
 *   @param pOi      pointer to file data struct, see oifile.h
 *   @param arrname  value of ARRNAME keyword
 *
 *   @return pointer to oi_array matching arrname, or NULL if no match
 */
oi_array *oi_fits_lookup_array(const oi_fits *pOi, const char *arrname)
{
  return (oi_array *) g_hash_table_lookup(pOi->arrayHash, arrname);
}

/**
 * Lookup array element corresponding to specified ARRNAME & STA_INDEX.
 *
 *   @param pOi       pointer to file data struct, see oifile.h
 *   @param arrname   value of ARRNAME keyword
 *   @param staIndex  value of STA_INDEX from data table
 *
 *   @return ptr to 1st element struct matching staIndex, or NULL if no match
 */
element *oi_fits_lookup_element(const oi_fits *pOi,
				const char *arrname, int staIndex)
{
  int i;
  oi_array *pArray;

  pArray = oi_fits_lookup_array(pOi, arrname);
  if (pArray == NULL) return NULL;
  /* We don't assume records are ordered by STA_INDEX */
  for(i=0; i<pArray->nelement; i++) {
    if(pArray->elem[i].sta_index == staIndex)
      return &pArray->elem[i];
  }
  return NULL;
}

/**
 * Lookup oi_wavelength corresponding to specified INSNAME.
 *
 *   @param pOi      pointer to file data struct, see oifile.h
 *   @param insname  value of INSNAME keyword
 *
 *   @return pointer to oi_wavelength matching insname, or NULL if no match
 */
oi_wavelength *oi_fits_lookup_wavelength(const oi_fits *pOi,
					 const char *insname)
{
  return (oi_wavelength *) g_hash_table_lookup(pOi->wavelengthHash, insname);
}

/**
 * Lookup oi_corr corresponding to specified CORRNAME.
 *
 *   @param pOi       pointer to file data struct, see oifile.h
 *   @param corrname  value of CORRNAME keyword
 *
 *   @return pointer to oi_wavelength matching insname, or NULL if no match
 */
oi_corr *oi_fits_lookup_corr(const oi_fits *pOi, const char *corrname)
{
  return (oi_corr *) g_hash_table_lookup(pOi->corrHash, corrname);
}

/**
 * Lookup target record corresponding to specified TARGET_ID.
 *
 *   @param pOi       pointer to file data struct, see oifile.h
 *   @param targetId  value of TARGET_ID from data table
 *
 *   @return ptr to 1st target struct matching targetId, or NULL if no match
 */
target *oi_fits_lookup_target(const oi_fits *pOi, int targetId)
{
  int i;

  /* We don't assume records are ordered by TARGET_ID */
  for(i=0; i<pOi->targets.ntarget; i++) {
    if(pOi->targets.targ[i].target_id == targetId)
      return &pOi->targets.targ[i];
  }
  return NULL;
}

  
/** Generate summary string for each oi_vis/vis2/t3 in GList. */
#define FORMAT_OI_LIST_SUMMARY(pGStr, list, type)               \
  {                                                             \
    int nn = 1;                                                 \
    GList *link = list;                                         \
    while(link != NULL) {                                       \
      g_string_append_printf(                                   \
        pGStr,                                                  \
        "    #%-2d DATE-OBS=%s\n"                               \
        "    INSNAME='%s'  ARRNAME='%s'  CORRNAME='%s'\n"       \
        "     %5ld records x %3d wavebands\n",                  \
        nn++,                                                   \
        ((type *) (link->data))->date_obs,                      \
        ((type *) (link->data))->insname,                       \
        ((type *) (link->data))->arrname,                       \
        ((type *) (link->data))->corrname,                      \
        ((type *) (link->data))->numrec,                        \
        ((type *) (link->data))->nwave);                        \
      link = link->next;                                        \
    }                                                           \
  }

/**
 * Generate file summary string.
 *
 * @param pOi  pointer to oi_fits struct
 *
 * @return String summarising supplied dataset
 */
const char *format_oi_fits_summary(const oi_fits *pOi)
{
  if (pGStr == NULL)
    pGStr = g_string_sized_new(512);

  g_string_printf(pGStr, "OIFITS data:\n");
  g_string_append_printf(pGStr, "  DATE-OBS=%s  OBJECT='%s'\n",
                         pOi->header.date_obs, pOi->header.object);
  g_string_append_printf(pGStr, "  TELESCOP='%s'  INSTRUME='%s'\n",
                         pOi->header.telescop, pOi->header.instrume);
  g_string_append_printf(pGStr, "  INSMODE='%s'  OBSTECH='%s'\n\n",
                         pOi->header.insmode, pOi->header.obstech);
  g_string_append_printf(pGStr, "  %d OI_ARRAY tables:\n", pOi->numArray);
  format_array_list_summary(pGStr, pOi->arrayList);
  g_string_append_printf(pGStr, "  %d OI_WAVELENGTH tables:\n",
			 pOi->numWavelength);
  format_wavelength_list_summary(pGStr, pOi->wavelengthList);
  g_string_append_printf(pGStr, "  %d OI_CORR tables:\n", pOi->numCorr);
  format_corr_list_summary(pGStr, pOi->corrList);
  g_string_append_printf(pGStr, "  %d OI_POLAR tables:\n", pOi->numPolar);
  format_polar_list_summary(pGStr, pOi->polarList);
  g_string_append_printf(pGStr, "  %d OI_VIS tables:\n", pOi->numVis);
  FORMAT_OI_LIST_SUMMARY(pGStr, pOi->visList, oi_vis);
  g_string_append_printf(pGStr, "  %d OI_VIS2 tables:\n", pOi->numVis2);
  FORMAT_OI_LIST_SUMMARY(pGStr, pOi->vis2List, oi_vis2);
  g_string_append_printf(pGStr, "  %d OI_T3 tables:\n", pOi->numT3);
  FORMAT_OI_LIST_SUMMARY(pGStr, pOi->t3List, oi_t3);
  g_string_append_printf(pGStr, "  %d OI_SPECTRUM tables:\n", pOi->numSpectrum);
  FORMAT_OI_LIST_SUMMARY(pGStr, pOi->spectrumList, oi_spectrum);

  return pGStr->str;
}

/**
 * Print file summary to stdout.
 *
 * @param pOi  pointer to oi_fits struct
 */
void print_oi_fits_summary(const oi_fits *pOi)
{
  printf("%s", format_oi_fits_summary(pOi));
}

/**
 * Make deep copy of a OI_TARGET table.
 *
 * @param pInTab  pointer to input table
 *
 * @return Pointer to newly-allocated copy of input table
 */
oi_target *dup_oi_target(const oi_target *pInTab)
{
  oi_target *pOutTab;

  MEMDUP(pOutTab, pInTab, sizeof(*pInTab));
  MEMDUP(pOutTab->targ, pInTab->targ,
	 pInTab->ntarget*sizeof(pInTab->targ[0]));
  return pOutTab;
}
/**
 * Make deep copy of a OI_ARRAY table.
 *
 * @param pInTab  pointer to input table
 *
 * @return Pointer to newly-allocated copy of input table
 */
oi_array *dup_oi_array(const oi_array *pInTab)
{
  oi_array *pOutTab;

  MEMDUP(pOutTab, pInTab, sizeof(*pInTab));
  MEMDUP(pOutTab->elem, pInTab->elem,
	 pInTab->nelement*sizeof(pInTab->elem[0]));
  return pOutTab;
}

/**
 * Make deep copy of a OI_WAVELENGTH table.
 *
 * @param pInTab  pointer to input table
 *
 * @return Pointer to newly-allocated copy of input table
 */
oi_wavelength *dup_oi_wavelength(const oi_wavelength *pInTab)
{
  oi_wavelength *pOutTab;

  MEMDUP(pOutTab, pInTab, sizeof(*pInTab));
  MEMDUP(pOutTab->eff_wave, pInTab->eff_wave,
	 pInTab->nwave*sizeof(pInTab->eff_wave[0]));
  MEMDUP(pOutTab->eff_band, pInTab->eff_band,
	 pInTab->nwave*sizeof(pInTab->eff_band[0]));
  return pOutTab;
}

/**
 * Make deep copy of a OI_CORR table.
 *
 * @param pInTab  pointer to input table
 *
 * @return Pointer to newly-allocated copy of input table
 */
oi_corr *dup_oi_corr(const oi_corr *pInTab)
{
  oi_corr *pOutTab;

  MEMDUP(pOutTab, pInTab, sizeof(*pInTab));
  MEMDUP(pOutTab->iindx, pInTab->iindx,
	 pInTab->ncorr*sizeof(pInTab->iindx[0]));
  MEMDUP(pOutTab->jindx, pInTab->jindx,
	 pInTab->ncorr*sizeof(pInTab->jindx[0]));
  MEMDUP(pOutTab->corr, pInTab->corr,
	 pInTab->ncorr*sizeof(pInTab->corr[0]));
  return pOutTab;
}

/**
 * Make deep copy of a OI_VIS table.
 *
 * @param pInTab  pointer to input table
 *
 * @return Pointer to newly-allocated copy of input table
 */
oi_vis *dup_oi_vis(const oi_vis *pInTab)
{
  oi_vis *pOutTab;
  oi_vis_record *pInRec, *pOutRec;
  int i;

  MEMDUP(pOutTab, pInTab, sizeof(*pInTab));
  MEMDUP(pOutTab->record, pInTab->record,
	 pInTab->numrec*sizeof(pInTab->record[0]));
  for(i=0; i<pInTab->numrec; i++) {
    pOutRec = &pOutTab->record[i];
    pInRec = &pInTab->record[i];
    MEMDUP(pOutRec->visamp, pInRec->visamp,
	   pInTab->nwave*sizeof(pInRec->visamp[0]));
    MEMDUP(pOutRec->visamperr, pInRec->visamperr,
	   pInTab->nwave*sizeof(pInRec->visamperr[0]));
    MEMDUP(pOutRec->visphi, pInRec->visphi,
	   pInTab->nwave*sizeof(pInRec->visphi[0]));
    MEMDUP(pOutRec->visphierr, pInRec->visphierr,
	   pInTab->nwave*sizeof(pInRec->visphierr[0]));
    MEMDUP(pOutRec->flag, pInRec->flag,
	   pInTab->nwave*sizeof(pInRec->flag[0]));
    if(pInTab->usevisrefmap)
    {
      MEMDUP(pOutRec->visrefmap, pInRec->visrefmap,
             pInTab->nwave*pInTab->nwave*sizeof(pInRec->visrefmap[0]));
    }
    if(pInTab->usecomplex)
    {
      MEMDUP(pOutRec->rvis, pInRec->rvis,
             pInTab->nwave*sizeof(pInRec->rvis[0]));
      MEMDUP(pOutRec->rviserr, pInRec->rviserr,
             pInTab->nwave*sizeof(pInRec->rviserr[0]));
      MEMDUP(pOutRec->ivis, pInRec->ivis,
             pInTab->nwave*sizeof(pInRec->ivis[0]));
      MEMDUP(pOutRec->iviserr, pInRec->rviserr,
             pInTab->nwave*sizeof(pInRec->iviserr[0]));
    }
  }
  return pOutTab;
}

/**
 * Make deep copy of a OI_VIS2 table.
 *
 * @param pInTab  pointer to input table
 *
 * @return Pointer to newly-allocated copy of input table
 */
oi_vis2 *dup_oi_vis2(const oi_vis2 *pInTab)
{
  oi_vis2 *pOutTab;
  oi_vis2_record *pInRec, *pOutRec;
  int i;

  MEMDUP(pOutTab, pInTab, sizeof(*pInTab));
  MEMDUP(pOutTab->record, pInTab->record,
	 pInTab->numrec*sizeof(pInTab->record[0]));
  for(i=0; i<pInTab->numrec; i++) {
    pOutRec = &pOutTab->record[i];
    pInRec = &pInTab->record[i];
    MEMDUP(pOutRec->vis2data, pInRec->vis2data,
	   pInTab->nwave*sizeof(pInRec->vis2data[0]));
    MEMDUP(pOutRec->vis2err, pInRec->vis2err,
	   pInTab->nwave*sizeof(pInRec->vis2err[0]));
    MEMDUP(pOutRec->flag, pInRec->flag,
	   pInTab->nwave*sizeof(pInRec->flag[0]));
  }
  return pOutTab;
}

/**
 * Make deep copy of a OI_T3 table.
 *
 * @param pInTab  pointer to input table
 *
 * @return Pointer to newly-allocated copy of input table
 */
oi_t3 *dup_oi_t3(const oi_t3 *pInTab)
{
  oi_t3 *pOutTab;
  oi_t3_record *pInRec, *pOutRec;
  int i;

  MEMDUP(pOutTab, pInTab, sizeof(*pInTab));
  MEMDUP(pOutTab->record, pInTab->record,
	 pInTab->numrec*sizeof(pInTab->record[0]));
  for(i=0; i<pInTab->numrec; i++) {
    pOutRec = &pOutTab->record[i];
    pInRec = &pInTab->record[i];
    MEMDUP(pOutRec->t3amp, pInRec->t3amp,
	   pInTab->nwave*sizeof(pInRec->t3amp[0]));
    MEMDUP(pOutRec->t3amperr, pInRec->t3amperr,
	   pInTab->nwave*sizeof(pInRec->t3amperr[0]));
    MEMDUP(pOutRec->t3phi, pInRec->t3phi,
	   pInTab->nwave*sizeof(pInRec->t3phi[0]));
    MEMDUP(pOutRec->t3phierr, pInRec->t3phierr,
	   pInTab->nwave*sizeof(pInRec->t3phierr[0]));
    MEMDUP(pOutRec->flag, pInRec->flag,
	   pInTab->nwave*sizeof(pInRec->flag[0]));
  }
  return pOutTab;
}

/**
 * Make deep copy of a OI_SPECTRUM table.
 *
 * @param pInTab  pointer to input table
 *
 * @return Pointer to newly-allocated copy of input table
 */
oi_spectrum *dup_oi_spectrum(const oi_spectrum *pInTab)
{
  oi_spectrum *pOutTab;
  oi_spectrum_record *pInRec, *pOutRec;
  int i;

  MEMDUP(pOutTab, pInTab, sizeof(*pInTab));
  MEMDUP(pOutTab->record, pInTab->record,
	 pInTab->numrec*sizeof(pInTab->record[0]));
  for(i=0; i<pInTab->numrec; i++) {
    pOutRec = &pOutTab->record[i];
    pInRec = &pInTab->record[i];
    MEMDUP(pOutRec->fluxdata, pInRec->fluxdata,
	   pInTab->nwave*sizeof(pInRec->fluxdata[0]));
    MEMDUP(pOutRec->fluxerr, pInRec->fluxerr,
	   pInTab->nwave*sizeof(pInRec->fluxerr[0]));
  }
  return pOutTab;
}
