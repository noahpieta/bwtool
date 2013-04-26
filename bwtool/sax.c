/* bwtool_sax - signal to symbol conversion */

#include "common.h"
#include "linefile.h"
#include "hash.h"
#include "options.h"
#include "sqlNum.h"
#include "basicBed.h"
#include "bigWig.h"
#include "bigs.h"
#include "bwtool.h"
#include "sax.h"

void usage_sax()
/* Explain usage of the sax program. */
{
errAbort(
  "bwtool sax - Implementation of SAX algorithm on bigWig data region.\n"
  "usage:\n"
  "   bwtool sax alphabet-size input.bw[:chr:start-end] output.sax\n"
  "where:\n"
  "   alphabet-size is from 2-20\n"
  "options:\n"
  "   -iterate-start=m\n"
  "   -iterate-end=n         run SAX algorithm with varying alphabet sizes\n"
  "                          ranging from m to n (inclusive).\n"
  "   -sax-window=n          run SAX using a window size of n.  n should be\n"
  "                          a power of 2 e.g. 16, 32, 1024, etc.\n"
  "   -force-bed4            when set, disable the FASTA output in favor of\n"
  "                          BED4 in the case of a single alphabet size\n"
  "   -add-wig-out           in the case of BED4 output, add an additional\n"
  "                          column that shows the original data\n"
  "   -mean=val              force z-normalization to use fixed mean\n"
  "   -std=val               force z-normalization to use fixed standard\n"
  "                          deviation\n"
  );
}

void wigsax_fasta(FILE *out, struct metaBig *mb, struct bed *region, int alpha, int window, double mean, double std)
/* when not using an iterative alphabet size, make an output similar to FASTA */
{
    struct perBaseWig *wigList = perBaseWigLoadContinue(mb->big.bbi, region->chrom, region->chromStart, region->chromEnd);
    struct perBaseWig *pbw;
    for (pbw = wigList; pbw != NULL; pbw = pbw->next)
    {
	int data_len = pbw->chromEnd-pbw->chromStart;
	char *sax = sax_from_array_force_window(pbw->data, data_len, alpha, window, mean, std);
	int i;
	fprintf(out, ">%s:%d-%d\n", pbw->chrom, pbw->chromStart, pbw->chromEnd);
	for (i = 0; i < data_len; i += 60)
	{
	    char swap = sax[i+60];
	    sax[i+60] = '\0';
	    fprintf(out, "%s\n", sax + i);
	    sax[i+60] = swap;
	}
	freeMem(sax);
    }
    perBaseWigFreeList(&wigList);
}

static struct bed *make_initial_bed_list(struct perBaseWig *pbw, int name_size)
/* make an initial list of beds to be filled in later with different SAX strings. */
{
    struct bed *bedList = NULL;
    int data_len = pbw->chromEnd - pbw->chromStart;
    int j;
    for (j = 0; j < data_len; j++)
    {
	struct bed *emptyBed;
	AllocVar(emptyBed);
	emptyBed->chrom = cloneString(pbw->chrom);
	emptyBed->chromStart = pbw->chromStart + j;
	emptyBed->chromEnd = emptyBed->chromStart + 1;
	AllocArray(emptyBed->name, name_size);
	emptyBed->name[name_size-1] = '\0';
	slAddHead(&bedList, emptyBed);
    }
    slReverse(&bedList);
    return bedList;
}

void wigsax_bed4(FILE *out, struct metaBig *mb, struct bed *region, int alphaS, int alphaE, int window, double mean, double std)
/* output the bed4 style when it's being run over an interval */
{
    struct bed *outBedList = NULL;
    struct bed *bed;
    struct perBaseWig *wigList = perBaseWigLoadContinue(mb->big.bbi, region->chrom, region->chromStart, region->chromEnd);
    struct perBaseWig *pbw;
    struct slDouble *datList = NULL;
    struct slDouble *oneDub;
    boolean outWig = optionExists("addWigOut");
    for (pbw = wigList; pbw != NULL; pbw = pbw->next)
    {
	struct bed *bedList = make_initial_bed_list(pbw, alphaE - alphaS + 2);
	int i, j;
	int data_len = pbw->chromEnd - pbw->chromStart; 
	for (i = alphaS; i <= alphaE; i++)
	{
	    char *sax = sax_from_array_force_window(pbw->data, data_len, i, window, mean, std);
	    for (j = 0, bed = bedList; ((j < data_len) && (bed != NULL)); j++, bed = bed->next)
		bed->name[i-alphaS] = sax[j];
	    freeMem(sax);
	}
	if (outWig)
	    for (j = 0; j < data_len; j++)
	    {
		struct slDouble *dub = newSlDouble(pbw->data[j]);
		slAddHead(&datList, dub);
	    }
	while ((bed = slPopHead(&bedList)) != NULL)
	    slAddHead(&outBedList, bed);
    }
    slReverse(&outBedList);
    slReverse(&datList);
    perBaseWigFreeList(&wigList);
    oneDub = datList;
    for (bed = outBedList; bed != NULL; bed = bed->next)
    {
	bedOutputN(bed, 4, out, '\t', (outWig) ? '\t' : '\n');
	if (outWig)
	{
	    if (oneDub == NULL)
		errAbort("data inconsistency. programmer error\n");
	    fprintf(out, "%0.4f\n", oneDub->val);
	    oneDub = oneDub->next;
	}
    }
    bedFreeList(&outBedList);
    slFreeList(&datList);
}

void bwtool_sax(struct hash *options, char *favorites, char *regions, unsigned decimals, char *alpha_s, char *bigfile,
		char *outputfile)
/* bwtool_sax - main for the sax symbol program */
{
    struct metaBig *mb = metaBigOpen_favs(bigfile, regions, favorites);
    struct bed *bed;
    int alpha = (alpha_s != NULL) ? sqlUnsigned(alpha_s) : 8;
    unsigned itStart = sqlUnsigned((char *)hashOptionalVal(options, "iterate-start", (alpha_s != NULL) ? alpha_s : "8"));
    unsigned itEnd = sqlUnsigned((char *)hashOptionalVal(options, "iterate-end", (alpha_s != NULL) ? alpha_s : "8"));
    unsigned window = sqlUnsigned((char *)hashOptionalVal(options, "sax-window", "0"));
    char *mean_s = (char *)hashOptionalVal(options, "mean", "mean");
    double mean = -DBL_MAX;
    if (!sameString(mean_s, "mean"))
	mean = sqlDouble(mean_s);
    double std = sqlDouble((char *)hashOptionalVal(options, "std", "0"));
    FILE *out;
    boolean do_std = (hashLookup(options, "std") != NULL);
    boolean do_mean = (hashLookup(options, "mean") != NULL);
    boolean force_bed4 = (hashLookup(options, "force-bed4") != NULL);
    if (do_mean || do_std)
    {
	if (!do_std || !do_mean)
	    errAbort("if -mean is specified, -std is required, and vice versa");
	else if (std <= 0)
	    errAbort("-std must be > 0");
    }
    out = mustOpen(outputfile, "w");
    for (bed = mb->sections; bed != NULL; bed = bed->next)
    {
	/* print a header */
	if (bed == mb->sections)
	    fprintf(out, "# created using wigsax version Sept2011.\n");
	if ((itStart == itEnd) && !force_bed4)
	{
	    if (bed == mb->sections)
		fprintf(out, "# alphabet size = %d\n", alpha);
	    wigsax_fasta(out, mb, bed, alpha, window, mean, std);
	}
	else
	{
	    if (bed == mb->sections)
		fprintf(out, "# alphabet size = %d-%d\n", itStart, itEnd);
	    wigsax_bed4(out, mb, bed, itStart, itEnd, window, mean, std);
	}
    }
    metaBigClose(&mb);
    carefulClose(&out);
}