/* List the content of a BP file.
 *
 * Copyright Oak Ridge National Laboratory 2009
 * Author: Norbert Podhorszki, pnorbert@ornl.gov
 *
 * TODO: -S handle int8[] as string
**/

#ifndef _GNU_SOURCE
#   define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>   // LONG_MAX
#include <math.h>     // NAN
#include <libgen.h>   // basename
#include <regex.h>    // regular expression matching
#include <fnmatch.h>  // shell pattern matching

#include "bpls.h"
#include "adios_read.h"
#include "adios_types.h"


#ifdef DMALLOC
#include "dmalloc.h"
#endif

// global variables 
// Values from the arguments or defaults
char *outpath;              // output files' starting path (can be extended with subdirs, names, indexes)
char *varmask[MAX_MASKS];   // can have many -var masks (either shell patterns or extended regular expressions)
char *grpmask;              // list only groups matching the mask
int  nmasks;                // number of masks specified
char *vfile;                // file name to bpls
char *start;                // dimension spec starting points 
char *count;                // dimension spec counts
char format[32];            // format string for one data element (e.g. %6.2f)
bool formatgiven;           // true if format string is provided as argument

// Flags from arguments or defaults
bool dump;                 // dump data not just list info
bool output_xml;
bool use_regexp;           // use varmasks as regular expressions
bool sortnames;            // sort names before listing
bool listattrs;            // do list attributes too
bool readattrs;            // also read all attributes and print
bool longopt;              // -l is turned on
bool noindex;              // do no print array indices with data
bool printByteAsChar;      // print 8 bit integer arrays as string

// other global variables
char *prgname; /* argv[0] */
//long timefrom, timeto;
int  istart[MAX_DIMS], icount[MAX_DIMS], ndimsspecified=0;
regex_t varregex[MAX_MASKS]; // compiled regular expressions of varmask
regex_t grpregex;            // compiled regular expressions of grpmask
int  ncols = 6; // how many values to print in one row (only for -p)
int  verbose = 0;
FILE *outf;   // file to print to or stdout
char commentchar;

struct option options[] = {
    {"help",                 no_argument,          NULL,    'h'},
    {"verbose",              no_argument,          NULL,    'v'},
    {"dump",                 no_argument,          NULL,    'd'},
    {"group",                no_argument,          NULL,    'g'},
    {"regexp",               no_argument,          NULL,    'e'},
    {"output",               required_argument,    NULL,    'o'},
    {"xml",                  no_argument,          NULL,    'x'},
    {"start",                required_argument,    NULL,    's'}, 
    {"count",                required_argument,    NULL,    'c'}, 
    {"noindex",              no_argument,          NULL,    'y'},
    {"sort",                 no_argument,          NULL,    't'},
    {"attrs",                no_argument,          NULL,    'a'},
    {"long",                 no_argument,          NULL,    'l'},
    {"string",               no_argument,          NULL,    'S'},
    {"columns",              required_argument,    NULL,    'n'}, 
    {"format",               required_argument,    NULL,    'f'}, 
//    {"time",                 required_argument,    NULL,    't'}, 
    {NULL,                   0,                    NULL,    0}
};


static const char *optstring = "hveytaldSg:o:x:s:c:n:f:";

// help function
void display_help() {
   //printf( "Usage: %s  \n", prgname);
   printf("usage: bpls [OPTIONS] file [mask1 mask2 ...]\n"
        "\nList/dump content of a BP file. \n"
        "A mask can be a shell pattern like with 'ls' e.g. \"*/x?\".\n"
        "Variables with multiple timesteps are reported with an extra dimensions.\n"
        "The time dimension is the first dimension then.\n"
        "\n"
        "  --long    | -l           Print values of all scalars and attributes and\n"
        "                           min/max values of arrays (no overhead to get them!)\n"
        "  --attrs   | -a           List/match attributes too\n"
        "  --sort    | -t           Sort names before listing\n"
        "  --group   | -g <mask>    List/dump groups matching the mask only\n"
        "  --dump    | -d           Dump matched variables/attributes\n"
        "                             To match attributes too, add option -a\n"
        "  --regexp  | -e           Treat masks as extended regular expressions\n"
        "  --output  | -o <path>    Print to a file instead of stdout\n"
/*
        "  --xml    | -x            # print as xml instead of ascii text\n"
*/
        "  --start   | -s \"spec\"    Offset indices in each dimension \n"
        "                             (default is 0 for all dimensions) \n"
        "                             <0 is handled as in python (-1 is last)\n"
        "  --count   | -c \"spec\"    Number of elements in each dimension\n"
        "                             -1 denotes 'until end' of dimension\n"
        "                             (default is -1 for all dimensions)\n"
        "  --noindex | -y           Print data without array indices\n"
        "  --string  | -S           Print 8bit integer arrays as strings\n"
        "  --columns | -n \"cols\"    Number of data elements per row to print\n"
        "  --format  | -f \"str\"     Format string to use for one data item in print\n"
        "                             instead of the default. E.g. \"%%6.3f\"\n"
/*
        "  --time    | -t N [M]      # print data for timesteps N..M only (or only N)\n"
        "                              default is to print all available timesteps\n"
*/
        "\n"
        "  Examples for slicing:\n"
        "  -s \"0,0,0\"    -c \"1,99,1\":  print 100 elements (of the 2nd dimension).\n"
        "  -s \"0,0\"      -c \"1,-1\":    print the whole 2nd dimension however large it is.\n"
        "  -s \"-1,-1\"    -c \"1,1\":     print the very last element (of a 2D array)\n"
        "\n"
        "Help options\n"
        "  --help    | -h               Print this help.\n"
        "  --verbose | -v               Print log about what this program is doing.\n"
        "                                 Use multiple -v to increase logging level.\n"
        "Typical use: bpls -lat <file>\n"
        );
}

/** Main */
int main( int argc, char *argv[] ) {
    int retval = 0;
    int i, timearg=false; 
    long int tmp;

    init_globals();

    ////prgname = strdup(argv[0]);

    /* other variables */
    int c, last_c='_';
    int last_opt = -1;
    /* Process the arguments */
    while ((c = getopt_long(argc, argv, optstring, options, NULL)) != -1) {
        switch (c) {
                case 'a':
                    listattrs=true;;
                    break;
                case 'c':
                    count = strndup(optarg,256);
                    break;
                case 'd':
                    dump = true;
                    break;
                case 'e':
                    use_regexp = true;
                    break;
                case 'f':
                    snprintf(format, sizeof(format), "%s", optarg);
                    formatgiven = true;
                    break;
                case 'g':
                    grpmask = strndup(optarg,256);
                    break;
                case 'h':
                    display_help();
                    return 0;
                case 'l':
                    longopt = true;
                    readattrs = true;
                    break;
                case 'n':
                    tmp = strtol(optarg, (char **)NULL, 0);
                    if (errno) {
                        fprintf(stderr, "Error: could not convert --columns value: %s\n", optarg);
                        return 1;
                    }
                    ncols=tmp;
                    break;
                case 'o':
                    outpath = strndup(optarg,256);
                    break;
                case 's':
                    start = strndup(optarg,256);
                    break;
                case 'S':
                    printByteAsChar = true;
                    break;
                case 't':
                    sortnames = true;
                    break;
                case 'x':
                    output_xml = true;
                    break;
                case 'y':
                    noindex = true;
                    break;
                case 'v':
                    verbose++;
                    break;
                /*
                case 't':
                    tmp = strtol(optarg, (char **)NULL, 0);
                    if (errno) {
                        fprintf(stderr, "Error: could not convert --time value: %s\n", optarg);
                        return 1;
                    }
                    timefrom = tmp; // 1st arg to --time
                    lastopt = 't';  // maybe there is a a 2nd arg too
                    break;
                */

                case 1:
                    /* This means a field is unknown, or could be multiple arg or bad arg*/
                    /*
                    if (last_c=='t') {  // --time 2nd arg (or not if not a number)
                        errno = 0;
                        tmp = strtol(optarg, (char **)NULL, 0);
                        if (!errno) {
                            timeto = tmp;
                            printf("Time set to %d - %d\n", timefrom, timeto);
                            timearg=true;
                        }
                    } 
                    */
                    if (!timearg) {
                        fprintf(stderr, "Unrecognized argument: %s\n", optarg);
                        return 1;
                    }
                    break;

                default:
                    printf("Processing default: %c\n", c);
                    break;
        } /* end switch */
        last_c = c;
    } /* end while */

    /* Check if we have a file defined */
    if (optind >= argc) {
        fprintf(stderr,"Missing file name\n");
        return 1;
    }
    vfile = strdup(argv[optind++]);
    while (optind < argc) {
        varmask[nmasks] = strndup(argv[optind++],256);
        nmasks++;
    }
 
    /* Process dimension specifications */
    if (start != NULL) parseDimSpec(start, istart);
    if (count != NULL) parseDimSpec(count, icount);

    // process the regular expressions
    if (use_regexp) {
        retval = compile_regexp_masks();
        if (retval)
            return retval;
    }

    if (noindex) commentchar = ';';
    else         commentchar = ' ';


    if (verbose>1) 
        printSettings();
   
    retval = print_start(outpath);
    if (retval)
        return retval;

    /* Start working */
    retval = doList(vfile);

    print_stop();

   /* Free allocated memories */
   //myfree(prgname);
   myfree(outpath);
   for (i=0; i<nmasks; i++) {
        myfree(varmask[i]);
        regfree(&(varregex[i]));
   }
   myfree(vfile);

   return retval; 
}

void init_globals(void) {
    int i;
    // variables for arguments
    outpath              = NULL;
    for (i=0; i<MAX_MASKS; i++)
        varmask[i]       = NULL;
    nmasks               = 0;
    vfile                = NULL;
    start                = NULL;
    count                = NULL;
    verbose              = 0;
    ncols                = 6;    // by default when printing ascii, print "X Y", not X: Y1 Y2...
    dump                 = false;
    output_xml           = false;
    noindex              = false;
    sortnames            = false;
    listattrs            = false;
    readattrs            = false;
    longopt              = false;
    //timefrom             = 1;
    //timeto               = -1;
    use_regexp           = false;
    formatgiven          = false;
    printByteAsChar      = false;
    for (i=0; i<MAX_DIMS; i++) {
        istart[i]  = 0;
        icount[i]  = -1;  // read full var by default
    }
    ndimsspecified = 0;
}


#define PRINT_DIMS(str, v, n, loopvar) printf("%s = { ", str); \
    for (loopvar=0; loopvar<n;loopvar++) printf("%lld ", v[loopvar]);    \
    printf("}")

void printSettings(void) {
    int i;
    printf("Settings :\n");
    printf("  masks  : %d ", nmasks);
    for (i=0; i<nmasks; i++)
        printf("%s ", varmask[i]);
    printf("\n");
    printf("  file   : %s\n", vfile);
    if (grpmask)
        printf("  groups : %s\n", grpmask);
    printf("  output : %s\n", (outpath ? outpath : "stdout"));

    if (start != NULL) {
        PRINT_DIMS("  start", istart, ndimsspecified,i); printf("\n");
    }
    if (count != NULL) {
        PRINT_DIMS("  count", icount, ndimsspecified,i); printf("\n");
    }

    if (longopt)
        printf("      -l : show scalar values and min/max of arrays\n");
    if (sortnames)
        printf("      -t : sort names before listing\n");
    if (listattrs)
        printf("      -a : list attributes too\n");
    if (dump)
        printf("      -d : dump matching variables and attributes\n");
    if (use_regexp)
        printf("      -e : handle masks as regular expressions\n");
    if (formatgiven)
        printf("      -f : dump using printf format \"%s\"\n", format);
    if (output_xml)
        printf("      -x : output data in XML format\n");
}

void bpexit(int code, ADIOS_FILE *fp, ADIOS_GROUP *gp) {
    if (gp > 0)
        adios_gclose (gp);
    if (fp > 0)
        adios_fclose (fp);
    exit(code);
}

void print_file_size(uint64_t size)
{
    static const int  sn=7;
    static const char *sm[]={"bytes", "KB", "MB", "GB", "TB", "PB", "EB"};
    uint64_t s = size, r;
    int idx = 0;
    while ( s/1024 > 0 ) {
        r = s%1024; 
        s = s/1024;
        idx++;
    }
    if (r > 511)
        s++;
    printf ("  file size:     %lld %s\n", s, sm[idx]); 

}

int doList(const char *path) {
    //int     ntsteps;

    ADIOS_FILE  *fp;
    ADIOS_GROUP *gp; // reused for each group
    ADIOS_VARINFO *vi; 
    enum ADIOS_DATATYPES vartype;
    int     grpid, i, j, n;             // loop vars
    int     status;
    int     attrsize;                       // info about one attribute
    int     mpi_comm_dummy;
    bool    matches;
    int     len, maxlen;
    int     nVarsMatched=0;
    int     nGroupsMatched=0;
    int     retval;
    char  **names;  // vars and attrs together, sorted or unsorted
    bool   *isVar;  // true for each var, false for each attr
    int     nNames; // number of vars + attrs
    void   *value;  // scalar value is returned by get_attr
    
    if (verbose>1) printf("\nADIOS BP open: read header info from %s\n", path);

    // open the BP file
    fp = adios_fopen (path, mpi_comm_dummy); 
    if (fp == NULL) {
	fprintf(stderr, "Error: %s\n", adios_errmsg());
	bpexit(7, 0, 0);
    }

    // get number of groups, variables, timesteps, and attributes 
    // all parameters are integers, 
    // besides the last parameter, which is an array of strings for holding the list of group names
    //ntsteps = fp->tidx_stop - fp->tidx_start + 1;
    if (verbose) {
         printf ("File info:\n");
         printf ("  of groups:     %d\n", fp->groups_count);
         printf ("  of variables:  %d\n", fp->vars_count);
         printf ("  of attributes: %d\n", fp->attrs_count);
         printf ("  time steps:    %d starting from %d\n", fp->ntimesteps, fp->tidx_start);
         print_file_size(fp->file_size);
         printf ("  bp version:    %d\n", fp->version);
         printf ("  endianness:    %s\n", (fp->endianness ? "Big Endian" : "Little Endian"));
         printf ("\n");
    }


    // each group has to be handled separately
    for (grpid=0; grpid < fp->groups_count; grpid++) {
        if (!grpMatchesMask(fp->group_namelist[grpid]))
            continue;
        nGroupsMatched++;
        if (!dump) fprintf(outf, "Group %s:\n", fp->group_namelist[grpid]);
        gp = adios_gopen_byid (fp, grpid);
        if (gp == NULL) {
	    fprintf(stderr, "Error: %s\n", adios_errmsg());
	    bpexit(8, fp, 0);
        }

        if (sortnames) {
            qsort(gp->var_namelist, gp->vars_count, sizeof(char *), cmpstringp);
            if (listattrs)
                qsort(gp->attr_namelist, gp->attrs_count, sizeof(char *), cmpstringp);
        }

        if (listattrs)
            nNames = gp->vars_count + gp->attrs_count;
        else 
            nNames = gp->vars_count;
        names = (char **) malloc (nNames * sizeof (char*)); // store only pointers
        isVar = (bool *) malloc (nNames * sizeof (bool));
        if (names == NULL || isVar == NULL) {
            fprintf(stderr, "Error: could not allocate char* and bool arrays of %d elements\n", nNames);
            return 5;
        }
        mergeLists(gp->vars_count, gp->var_namelist, gp->attrs_count, gp->attr_namelist,
                  names, isVar);

        // calculate max length of variable names in the first round
        maxlen = 4;
        for (n=0; n<nNames; n++) {
            len = strlen(names[n]);
            if (len > maxlen) maxlen = len;
        }
        
        /* VARIABLES */
        for (n=0; n<nNames; n++) {
            matches = false;
            if (isVar[n])  {
                vi = adios_inq_var (gp, names[n]);
                if (!vi) {
                    fprintf(stderr, "Error: %s\n", adios_errmsg());
                }
                vartype = vi->type;
            } else {
                retval = adios_get_attr (gp, names[n], &vartype, &attrsize, &value);
                if (retval) {
                    fprintf(stderr, "Error: %s\n", adios_errmsg());
                }
            }

            matches = matchesAMask(names[n]);

            if (matches) {
                nVarsMatched++;
                // print definition of variable
                fprintf(outf,"%c %-*s  %-*s", commentchar, 9, adios_type_to_string(vartype), maxlen, names[n]); 
                if (!isVar[n]) {
                    // list (and print) attribute
                    if (readattrs || dump) {
                        fprintf(outf,"  attr   = ");
                        print_data(value, 0, vartype, false); 
                        fprintf(outf,"\n");
                        matches = false; // already printed
                    } else {
                        fprintf(outf,"  attr\n");
                    }
                } else if (!vi) { 
                    // after error
                    fprintf(outf, "\n");
                } else if (vi->ndim > 0) {
                    // array
                    //fprintf(outf,"  {%s%d", (vi->timedim==0 ? "T-": ""),vi->dims[0]);
                    fprintf(outf,"  {%lld", vi->dims[0]);
                    for (j=1; j < vi->ndim; j++) {
                       fprintf(outf,", %lld", vi->dims[j]);
                    }
                    fprintf(outf,"}");
                    if (longopt && vi->gmin && vi->gmax) {
                       fprintf(outf," = ");
                       print_data(vi->gmin, 0, vartype, false); 
                       fprintf(outf,"/ ");
                       print_data(vi->gmax, 0, vartype, false); 
                    }
                    fprintf(outf,"\n");
                } else {
                    // scalar
                    fprintf(outf,"  scalar");
                    if (longopt && vi->value) {
                        fprintf(outf," = ");
                        print_data(vi->value, 0, vartype, false); 
                        matches = false; // already printed
                    }
                    fprintf(outf,"\n");
                }
            }

            if (matches && dump) {
                // print variable content 
                if (isVar[n])
                    retval = readVar(gp, vi);
                if (retval)
                    return retval;
                fprintf(outf,"\n");
            }

            if (isVar[n])
                adios_free_varinfo(vi);
            else
                free(value);
        }
        adios_gclose (gp);
        free(names);
        free(isVar);
    }

    if (grpmask != NULL && nGroupsMatched == 0) {
        fprintf(stderr, "\nError: None of the groups matched the group mask you provided: %s\n", grpmask);
        return 4;
    }
    if (nmasks > 0 && nVarsMatched == 0) {
        fprintf(stderr, "\nError: None of the variables matched any name/regexp you provided\n");
        return 4;
    }
    adios_fclose (fp);
    return 0;
}                


int cmpstringp(const void *p1, const void *p2)
{
    /* The actual arguments to this function are "pointers to
       pointers to char", but strcmp() arguments are "pointers
       to char", hence the following cast plus dereference */
    return strcmp(* (char * const *) p1, * (char * const *) p2);
}
/** Merge listV with listA if listattrs=true, otherwise just return listV.
    If sortnames=true, quicksort the result list.
*/
void mergeLists(int nV, char **listV, int nA, char **listA, char **mlist, bool *isVar) 
{
    int v, a;
    if (sortnames && listattrs) {
        // merge sort the two lists
        v = 0;
        a = 0;
        while (v < nV || a < nA) {
            if (a < nA && (v >= nV || strcmp(listV[v], listA[a]) > 0)) {
                // fully consumed var list or 
                // next item in attr list is less than next item in var list
                mlist[v+a] = listA[a];
                isVar[v+a] = false;
                a++;
            } else {
                mlist[v+a] = listV[v];
                isVar[v+a] = true;
                v++;
            }
        }
    } else {
        // first add vars then attrs (if asked)
        for (v=0; v<nV; v++) {
                mlist[v] = listV[v];
                isVar[v] = true;
        }
        if (listattrs) {
            for (a=0; a<nA; a++) {
                    mlist[a+nV] = listA[a];
                    isVar[a+nV] = false;
            }
        }
    }
}

int getTypeInfo( enum ADIOS_DATATYPES adiosvartype, int* elemsize)
{
    switch(adiosvartype) {
        case adios_unsigned_byte:
            *elemsize = 1;
            break;
        case adios_byte:
            *elemsize = 1;
            break;
        case adios_string:
            *elemsize = 1;
            break;
            
        case adios_unsigned_short:  
            *elemsize = 2;
            break;
        case adios_short:
            *elemsize = 2;
            break;
            
        case adios_unsigned_integer:
            *elemsize = 4;
            break;
        case adios_integer:    
            *elemsize = 4;
            break;

        case adios_unsigned_long:
            *elemsize = 8;
            break;
        case adios_long:        
            *elemsize = 8;
            break;

        case adios_real:
            *elemsize = 4;
            break;

        case adios_double:
            *elemsize = 8;
            break;

        case adios_complex:  
            *elemsize = 8;
            break;

        case adios_double_complex:
            *elemsize = 16;
            break;

        case adios_long_double: // do not know how to print
            // *elemsize = 16;
        default:
	    return 1;
    }
    return 0;
}

/** Read data of a variable and print 
  * Return: 0: ok, != 0 on error
  */
int readVar(ADIOS_GROUP *gp, ADIOS_VARINFO *vi)
{
    int i,j;
    uint64_t start_t[MAX_DIMS], count_t[MAX_DIMS]; // processed <0 values in start/count
    uint64_t s[MAX_DIMS], c[MAX_DIMS]; // for block reading of smaller chunks
    uint64_t nelems;         // number of elements to read
    int elemsize;            // size in bytes of one element
    uint64_t st, ct;
    void *data;
    uint64_t sum;           // working var to sum up things
    int  maxreadn;          // max number of elements to read once up to a limit (10MB of data)
    int  actualreadn;       // our decision how much to read at once
    int  readn[MAX_DIMS];   // how big chunk to read in in each dimension?
    int64_t bytes_read;     // retval from adios_get_var()
    bool incdim;            // used in incremental reading in

    char *name = gp->var_namelist[vi->varid];

    if (getTypeInfo(vi->type, &elemsize)) {
        fprintf(stderr, "Adios type %d (%s) not supported in bpls. var=%s\n", 
                vi->type, adios_type_to_string(vi->type), name);
        return 10;
    }

    // create the counter arrays with the appropriate lengths
    // transfer start and count arrays to format dependent arrays

    nelems = 1;
    for (j=0; j<vi->ndim; j++) {
        if (istart[j] < 0)  // negative index means last-|index|
            st = vi->dims[j]+istart[j];
        else
            st = istart[j];
        if (icount[j] < 0)  // negative index means last-|index|+1-start
            ct = vi->dims[j]+icount[j]+1-st;
        else
            ct = icount[j];

        if (verbose>2) 
            printf("    j=%d, st=%d ct=%d\n", j, st, ct);

        start_t[j] = st;
        count_t[j] = ct;
        nelems *= ct;
        if (verbose>1) 
            printf("    s[%d]=%d, c[%d]=%d, n=%lld\n", j, start_t[j], j, count_t[j], nelems);
    }

    if (verbose>1) {
        printf(" total size of data to read = %lld\n", nelems*elemsize);
    }

    print_slice_info(vi->ndim, vi->dims, start_t, count_t);
    
    maxreadn = MAX_BUFFERSIZE/elemsize;
    if (nelems < maxreadn)
        maxreadn = nelems;
   
    // allocate data array
    data = (void *) malloc (maxreadn*elemsize+8); // +8 for just to be sure

    // determine strategy how to read in:
    //  - at once
    //  - loop over 1st dimension
    //  - loop over 1st & 2nd dimension
    //  - etc
    if (verbose>1) printf("Read size strategy:\n");
    sum = (uint64_t) 1;
    actualreadn = (uint64_t) 1;
    for (i=vi->ndim-1; i>=0; i--) {
        if (sum >= (uint64_t) maxreadn) {
            readn[i] = 1;
        } else {
            readn[i] = maxreadn / (int)sum; // sum is small for 4 bytes here
            // this may be over the max count for this dimension
            if (readn[i] > count_t[i]) 
                readn[i] = count_t[i];
        }
        if (verbose>1) printf("    dim %d: read %d elements\n", i, readn[i]);
        sum = sum * (uint64_t) count_t[i];
        actualreadn = actualreadn * readn[i];
    }
    if (verbose>1) printf("    read %d elements at once, %lld in total (nelems=%lld)\n", actualreadn, sum, nelems);


    // init s and c
    for (j=0; j<vi->ndim; j++) {
        s[j]=start_t[j];
        c[j]=readn[j];
    }

    // read until read all 'nelems' elements
    sum = 0;
    while (sum < nelems) {
        
        // how many elements do we read in next?
        actualreadn = 1;
        for (j=0; j<vi->ndim; j++) 
            actualreadn *= c[j];

        if (verbose>2) {
            printf("adios_read_var name=%s ", name);
            PRINT_DIMS("  start", s, vi->ndim, j); 
            PRINT_DIMS("  count", c, vi->ndim, j); 
            printf("  read %d elems\n", actualreadn);
        }

        // read a slice finally
        bytes_read = adios_read_var_byid (gp, vi->varid, s, c, data); 

        if (bytes_read < 0) {
            fprintf(stderr, "Error when reading variable %s. errno=%d : %s \n", name, bytes_read, adios_errmsg());
            free(data);
            return 11;
        }

        if (verbose>2) printf("  read %lld bytes\n", bytes_read);

        // print slice
        print_dataset(data, vi->type, s, c, vi->ndim, vi->dims); 

        // prepare for next read
        sum += actualreadn;
        incdim=true; // largest dim should be increased 
        for (j=vi->ndim-1; j>=0; j--) {
            if (incdim) {
                if (s[j]+c[j] == start_t[j]+count_t[j]) {
                    // reached the end of this dimension
                    s[j] = start_t[j];
                    c[j] = readn[j];
                    incdim = true; // next smaller dim can increase too
                } else {
                    // move up in this dimension up to total count
                    s[j] += readn[j];
                    if (s[j]+c[j] > start_t[j]+count_t[j]) {
                        // do not reach over the limit
                        c[j] = start_t[j]+count_t[j]-s[j];
                    }
                    incdim = false;
                }
            }
        }
    } // end while sum < nelems
    print_endline();
    

    free(data);
    return 0;
}


bool matchesAMask(char *name) {
    int excode;
    int i;
    int startpos=0; // to match with starting / or without
    regmatch_t pmatch[1] = {{ (regoff_t) -1, (regoff_t) -1}};

    if (nmasks == 0) 
        return true;

    for (i=0; i<nmasks; i++) {
        if (use_regexp) {
            excode = regexec (&(varregex[i]), name, 1, pmatch, 0);
            if (name[0] == '/') // have to check if it matches from the second character too
                startpos = 1;
            if (excode == 0 &&                  // matches
                (pmatch[0].rm_so == 0 || pmatch[0].rm_so == startpos) &&         // from the beginning
                pmatch[0].rm_eo == strlen(name) // to the very end of the name
               ) {
                if (verbose>1)
                    printf("Name %s matches regexp %i %s\n", name, i, varmask[i]);
                //printf("Match from %d to %d\n", (int) pmatch[0].rm_so, (int) pmatch[0].rm_eo);
                return true;
            }
        } else {
            // use shell pattern matching
            if (varmask[i][0] != '/' && name[0] == '/')
                startpos = 1;
            if ( fnmatch( varmask[i], name+startpos, FNM_FILE_NAME) == 0) {
                if (verbose>1)
                    printf("Name %s matches varmask %i %s\n", name, i, varmask[i]);
                return true; 
            }
        }
    }
    return false;
}

/* return true if mask is null */
bool grpMatchesMask(char *name) {
    int startpos=0;
    int excode;
    regmatch_t pmatch[1] = {{ (regoff_t) -1, (regoff_t) -1}};

    if (grpmask == NULL)
        return true;

    if (use_regexp) {
        excode = regexec (&(grpregex), name, 1, pmatch, 0);
        if (name[0] == '/') // have to check if it matches from the second character too
            startpos = 1;
        if (excode == 0 &&                  // matches
            (pmatch[0].rm_so == 0 || pmatch[0].rm_so == startpos) &&         // from the beginning
            pmatch[0].rm_eo == strlen(name) // to the very end of the name
            ) {
            if (verbose>1)
                printf("Name %s matches regexp %s\n", name, grpmask);
            //printf("Match from %d to %d\n", (int) pmatch[0].rm_so, (int) pmatch[0].rm_eo);
            return true;
        }
    } else {
        // use shell pattern matching
        if (grpmask[0] != '/' && name[0] == '/')
            startpos = 1;
        if ( fnmatch( grpmask, name+startpos, FNM_FILE_NAME) == 0) {
            if (verbose>1)
                printf("Name %s matches groupmask %s\n", name, grpmask);
            return true; 
        }
    }
    return false;
}


int  print_start(const char *fname) {
    if ( fname == NULL) {
        outf = stdout;
    } else {
        if ((outf = fopen(fname,"w")) == NULL) {
            fprintf(stderr, "Error at opening for writing file %s: %s\n",
                    fname, strerror(errno));
            return 30;
        }
    }
    return 0;
}

void print_stop() {
    fclose(outf);
}

static int nextcol=0;  // column index to start with (can have lines split in two calls)

void print_slice_info(int ndim, uint64_t *dims, uint64_t *s, uint64_t *c)
{
    // print the slice info in indexing is on and 
    // not the complete variable is read
    int i;
    bool isaslice = false;
    for (i=0; i<ndim; i++) {
        if (c[i] < dims[i])
            isaslice = true;
    }
    if (isaslice) {
        fprintf(outf,"%c   slice (%lld:%lld", commentchar, s[0], s[0]+c[0]-1);
        for (i=1; i<ndim; i++) {
            fprintf(outf,", %lld:%lld", s[i], s[i]+c[i]-1);
        }
        fprintf(outf,")\n");
    }
}

int print_data_as_string(void * data, int maxlen, enum ADIOS_DATATYPES adiosvartype)
{
    char *str = (char *)data;
    int len = maxlen;
    bool cstring = false;
    switch(adiosvartype) {
        case adios_unsigned_byte:
        case adios_byte:
        case adios_string:
            while ( str[len-1] == 0) { len--; }   // go backwards on ascii 0s
            if (len < maxlen) {
                // it's a C string with terminating \0
                fprintf(outf,"\"%s\"", str);
            } else {
                // fortran VARCHAR, lets trim from right padded zeros
                while ( str[len-1] == ' ') {len--;}
                fprintf(outf,"\"%*.*s\"", len, len, (char *) data);
                if (len < maxlen)
                    fprintf(outf," + %d spaces", maxlen-len);
            }
            break;
        default:
            fprintf(stderr, "Error in bpls code: cannot use print_data_as_string() for type \"%s\"\n", adios_type_to_string);
            return -1;
            break;
    }
    return 0;
}

int print_data(void *data, int item, enum ADIOS_DATATYPES adiosvartype, bool allowformat)
{
    bool f = formatgiven && allowformat;
    if (data == NULL) {
        fprintf(outf, "null ");
        return 0;
    }
    // print next data item into vstr
    switch(adiosvartype) {
        case adios_unsigned_byte:
            fprintf(outf,(f ? format : "%hhu "), ((unsigned char *) data)[item]);
            break;
        case adios_byte:
            fprintf(outf,(f ? format : "%hhd "), ((char *) data)[item]);
            break;
        case adios_string:
            fprintf(outf,(f ? format : "\"%s\""), ((char *) data)+item);
            break;
    
        case adios_unsigned_short:  
            fprintf(outf,(f ? format : "%hu "), ((unsigned short *) data)[item]);
            break;
        case adios_short:
            fprintf(outf,(f ? format : "%hd "), ((short *) data)[item]);
            break;
    
        case adios_unsigned_integer:
            fprintf(outf,(f ? format : "%u "), ((unsigned int *) data)[item]);
            break;
        case adios_integer:    
            fprintf(outf,(f ? format : "%d "), ((int *) data)[item]);
            break;

        case adios_unsigned_long:
            fprintf(outf,(f ? format : "%llu "), ((unsigned long long *) data)[item]);
            break;
        case adios_long:        
            fprintf(outf,(f ? format : "%lld "), ((long long *) data)[item]);
            break;

        case adios_real:
            fprintf(outf,(f ? format : "%g "), ((float *) data)[item]);
            break;

        case adios_double:
            fprintf(outf,(f ? format : "%g "), ((double *) data)[item]);
            break;

        /*
        case adios_long_double:
            fprintf(outf,(f ? format : "%g "), ((double *) data)[item]);
            break;
        */

        case adios_complex:  
            fprintf(outf,(f ? format : "(%g,i%g) "), ((float *) data)[2*item], ((float *) data)[2*item+1]);
            break;

        case adios_double_complex:
            fprintf(outf,(f ? format : "(%g,i%g)" ), ((double *) data)[2*item], ((double *) data)[2*item+1]);
            break;
    } // end switch
    return 0;
}

int print_dataset(void *data, enum ADIOS_DATATYPES adiosvartype, 
                uint64_t *s, uint64_t *c, int ndim, uint64_t *dims)
{
    int i,item, steps;
    char idxstr[128], vstr[128], buf[16];
    uint64_t ids[MAX_DIMS];  // current indices
    bool roll;


    // init current indices
    steps = 1;
    for (i=0; i<ndim; i++) {
        ids[i] = s[i];
        steps *= c[i];
    }
    
    item = 0; // index to *data 
    // loop through each data item and print value
    while (item < steps) {

        // print indices if needed into idxstr;
        idxstr[0] = '\0'; // empty idx string
        if (nextcol == 0) {
            if (!noindex && ndim > 0) {
                sprintf(idxstr,"  (%lld",ids[0]);
                for (i=1; i<ndim; i++) {
                    sprintf(buf,",%lld",ids[i]);
                    strcat(idxstr, buf);
                }
                strcat(idxstr,")    ");
            }
        }

        // print item
        fprintf(outf, "%s", idxstr);
        if (printByteAsChar && (adiosvartype == adios_byte || adiosvartype == adios_unsigned_byte)) {
            print_data_as_string(data, steps, adiosvartype);
            nextcol = 0;
            break; // break the while loop;
        } else {
            print_data(data, item, adiosvartype, true);
        }

        // increment/reset column index
        nextcol++;
        if (nextcol == ncols) {
            fprintf(outf,"\n");
            nextcol = 0;
        }
            
        // increment indices
        item++;
        roll = true;
        for (i=ndim-1; i>=0; i--) {
            if (roll) {
                if (ids[i] == s[i]+c[i]-1 ) {
                    // last index in this dimension, roll upward
                    ids[i] = s[i];
                } else {
                    ids[i]++;
                    roll = false;
                }
            }
        }
    }
}

void print_endline(void) 
{
    if (nextcol != 0)
        fprintf(outf,"\n");
    nextcol = 0;
}



// parse a string "0, 3; 027" into an integer array
// of [0,3,27] 
// exits if parsing failes
void parseDimSpec(char *str, int *dims)
{
    char *token, *saveptr;
    char *s;  // copy of s; strtok modifies the string
    int  i=0;

    s = strndup(str, 256);
    token = strtok_r(s, " ,;x\t\n", &saveptr);
    while (token != NULL && i < MAX_DIMS) {
        //printf("\t|%s|", token);
        errno = 0;
        dims[i] = strtol(token, (char **)NULL, 0);
        if (errno) {
            fprintf(stderr, "Error: could not convert field into a value: %s from \"%s\"\n", token, str);
            exit(200);
        }

        // get next item
        token = strtok_r(NULL, " ,;x\t\n", &saveptr);
        i++;
    }
    //if (i>0) printf("\n");

    if (i > ndimsspecified) ndimsspecified = i;

    // check if number of dims specified is larger than we can handle
    if (token != NULL) {
        fprintf(stderr, "Error: More dimensions specified in \"%s\" than we can handle (%d)\n", str, MAX_DIMS);
        exit(200);
    }
}

int compile_regexp_masks(void)
{
    int i, errcode;
    char buf[256];
    for (i=0; i<nmasks; i++) {
        errcode = regcomp( &(varregex[i]), varmask[i], REG_EXTENDED);
        if (errcode) {
            regerror(errcode, &(varregex[i]), buf, sizeof(buf));
            fprintf(stderr, "Error: var %s is an invalid extended regular expression: %s\n", varmask[i], buf);
            return 2;
        }
    }
    if (grpmask != NULL) {
        errcode = regcomp( &(grpregex), grpmask, REG_EXTENDED);
        if (errcode) {
            regerror(errcode, &(grpregex), buf, sizeof(buf));
            fprintf(stderr, "Error: var %s is an invalid extended regular expression: %s\n", grpmask, buf);
            return 2;
        }
    }
    return 0;
}
