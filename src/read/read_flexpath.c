/*
    read_flexpath.c
    
    Originally copied from read_datatap.c
    Goal: to create evpath io connection layer in conjunction with 
    write/adios_flexpath.c

*/





// system libraries
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/times.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>

// evpath libraries
#include <ffs.h>
#include <atl.h>
#include <gen_thread.h>
#include <evpath.h>

// local libraries
#include "config.h"
#include "public/adios.h"
#include "public/adios_read_v2.h"
#include "core/adios_read_hooks.h"
#include "core/adios_logger.h"
#include "public/adios_error.h"
#include "core/flexpath.h"

// conditional libraries
#ifdef DMALLOC
#include "dmalloc.h"
#endif

/*
 * Contains start & counts for each dimension for each writer.
 */
typedef struct _array_displ
{
    int writer_rank;
    int ndims;
    int * start;
    int * count;    
}array_displacements;

typedef struct _bridge_info
{
    EVstone bridge_stone;
    EVsource flush_source;
    EVsource var_source;
    EVsource op_source;
    int their_num;
    char * contact;
    int created;
    int opened;
    int step;
}bridge_info;

typedef struct _flexpath_var_chunk
{
    int has_data;
    int rank;
    void *data;
    uint64_t *local_bounds; // nodims
    uint64_t *global_bounds; // ndims
    uint64_t *global_offsets; // ndims
    struct _flexpath_var_chunk *next;
} flexpath_var_chunk, *flexpath_var_chunk_p;

typedef struct _flexpath_var_info
{
    int id;
    char *varname;
    char *varpath;
    enum ADIOS_DATATYPES type;
    //add selector typ
    uint64_t data_size; // type size, not arrays size
    int time_dim; // -1 means no time dimension
    int ndims;
    int * dims; // ndims size (if ndims>0)
    uint64_t array_size; // not relevant for scalars
    int num_chunks;
    int was_scheduled;
    flexpath_var_chunk *chunks;
    int num_displ;
    array_displacements * displ;
    const ADIOS_SELECTION * sel;
    struct _flexpath_var_info *next;
} flexpath_var_info, *flexpath_var_info_p;

typedef struct _flexpath_file_data
{
    char * file_name;
    char * group_name; // assuming one group per file right now.

    EVstone ctrl_stone;
    EVstone split_stone; // to be added to ctrl_stone
    EVaction split_action;
    EVstone data_stone;

    MPI_Comm comm;
    int rank;
    int size;
    int valid;

    int num_bridges;
    bridge_info *bridges;
    FMFormat current_format;
    FMContext context;

    int writer_coordinator;

    int num_vars;
    flexpath_var_info * var_list;
    int num_gp; // for array distribution.
    int valid_evgroup;
    evgroup * gp;

    char *arr;
    char *rep_id;
    int rep_id_len;
    int id_len;

    int* sendees;
    int num_sendees;
    int ackCondition;    
} flexpath_file_data;


flexpath_file_data *
new_flexpath_file_data(const char * fname);

flexpath_var_info*
new_flexpath_var_info(const char * varname, int id, uint64_t data_size);

flexpath_var_info*
new_flexpath_var_info(const char * varname, int id, uint64_t data_size)
{
    flexpath_var_info * var = malloc(sizeof(flexpath_var_info));
    if(var == NULL){
	log_error("Error creating new var: %s\n", varname);
	exit(1);
    }
    
    var->varname = strdup(varname);
    var->id = id;
    var->data_size = data_size;
    var->chunks = NULL;
    var->sel = NULL;
    var->dims = NULL;
    var->displ = NULL;
    var->was_scheduled = 0;    
    var->time_dim = 0;
    var->ndims = 0;
    var->next = NULL;
    return var;
}


flexpath_file_data*
new_flexpath_file_data(const char * fname)
{
    flexpath_file_data * fp = malloc(sizeof(flexpath_file_data));
    if(fp == NULL){
	log_error("Cannot create data for new file.\n");
	exit(1);
    }
    fp->file_name = strdup(fname);
    fp->group_name = NULL;    
    fp->var_list = NULL;
    fp->gp = NULL;
    fp->bridges = NULL;
    fp->current_format = NULL;
    fp->context = NULL;
    
    fp->writer_coordinator = -1;
    fp->valid = 0;
    fp->num_bridges = 0;
    fp->num_gp = 0;
    fp->valid_evgroup = 0;
    fp->num_vars = 0;
    fp->sendees = NULL;
    fp->num_sendees = 0;    

    fp->arr = NULL;
    fp->rep_id = NULL;
    fp->rep_id_len = 0;
    fp->id_len = 0;
    int ackCondition = 0;
    return fp;        
}

//flexpath_file_data * file_data_list = NULL;

typedef struct _local_read_data
{
    // MPI stuff
    MPI_Comm fp_comm;
    int fp_comm_rank;
    int fp_comm_size;

    // EVPath stuff
    CManager fp_cm;
    EVstone ctrl_stone;
    EVstone data_stone;
    atom_t CM_TRANSPORT;

    // server state
    int fp_server_ready;
    int num_io_dumps;
    // TODO: timestep

} flexpath_read_data, *flexpath_read_data_p;


static int compare_var_name(const char* varname, const flexpath_var_info *v);
// this sructure holds all global data for flexpath read  methods
flexpath_read_data* fp_read_data = NULL;
//int ackCondition;

#define VAR_BITMAP_SIZE 16

ADIOS_VARINFO*
convert_file_info(flexpath_var_info * current_var,
		  ADIOS_VARINFO * v,
		  const char* varname,
		  const ADIOS_FILE* gp);

flexpath_var_info *
find_fp_var(flexpath_var_info * var_list, const char * varname)
{
    while(var_list){
	if(!compare_var_name(varname, var_list)){
	    return var_list;
	}
	else
	    var_list = var_list->next;
    }
    return NULL;
}

// compare used-providd varname with the full path name of variable v
// return zero if matches and non-zero otherwise
static int
compare_var_name (const char *varname, const flexpath_var_info *v)
{
    if (varname[0] == '/') { // varname is full path
        char fullpath[256];
        if(!strcmp(v->varpath, "/")) {
            sprintf(fullpath, "/%s", v->varname);
        }
        else {
            sprintf(fullpath, "%s/%s", v->varpath, v->varname);
        }
        return strcmp(fullpath, varname);
    }
    else { // varname doesn't include path
        return strcmp(v->varname, varname);
    }
}

global_var* 
find_gbl_var(global_var * vars, char * name, int num_vars)
{
    global_var * retvar = NULL;
    int i;
    for(i=0; i<num_vars; i++){
	if(!strcmp(vars[i].name, name))
	    return &vars[i];
    }
    return retvar;
}

static FMField
*find_field (const char *name, const FMFieldList flist)
{
    FMField *f = flist;
    while (f->field_name != NULL)
    {
        if(!strcmp(name, f->field_name))
            return f;
        else
            f++;
    }
    return NULL;
}



static int op_msg_handler(CManager cm, void *vevent, void *client_data, attr_list attrs) {
    op_msg* msg = (op_msg*)vevent;    
    ADIOS_FILE *adiosfile = (ADIOS_FILE*)client_data;
    flexpath_file_data *fp = (flexpath_file_data*)adiosfile->fh;
    if(msg->type==2) {
        CMCondition_signal(fp_read_data->fp_cm, msg->condition);
        //ackCondition = CMCondition_get(fp_read_data->fp_cm, NULL);
    }
    if(msg->type == 4){	
	adios_errno = err_end_of_stream;
	CMCondition_signal(fp_read_data->fp_cm, msg->condition);
    }       
    return 0;
}

/*
 * Should only be invoked from rank 0.  might need a better way to go about this.
 */
static int
group_msg_handler(CManager cm, void *vevent, void *client_data, attr_list attrs)
{
    //EVtake_event_buffer(fp_read_data->fp_cm, vevent);
    evgroup * msg = (evgroup*)vevent;
    ADIOS_FILE *adiosfile = client_data;
    flexpath_file_data * fp = (flexpath_file_data*)adiosfile->fh;
    fp->gp = msg;
    fp->valid_evgroup = 1;
    global_var * vars = msg->vars;
    int num_vars = msg->num_vars;
    CMCondition_signal(fp_read_data->fp_cm, msg->condition);    
    return 0;

}


void 
print_int_arr(char * tag, int * arr, int count)
{
    int i;
    log_debug_cont("%s: ", tag);
    for(i=0; i<count; i++){
	log_debug_cont("%d ", arr[i]);
    }
    log_debug("\n");
}

array_displacements*
find_displacement(array_displacements* list, int rank, int num_displ){
    int i;
    for(i=0; i<num_displ; i++){
	if(list[i].writer_rank == rank)
	    return &list[i];	
    }
    return NULL;
}

int
linearize_displ(int * offset, int * sizes, int ndim, int data_size)
{
    //print_int_arr("linearize_displ offsets", offset, ndim);
    //print_int_arr("linearize_displ sizes", sizes, ndim);
    int i;
    int retval = 0;
    for(i = 0; i<ndim - 1; i++){
	retval += (offset[i] * sizes[i+1])*data_size;       
    }
    retval+=offset[ndim-1]*data_size;
    return retval;
}


void 
copyoffsets(int dim, // dimension index
	    int ndims, // number of dimensions
	    int data_size, // data size
	    int* disp_start, // start array from array_displacements struct
	    int* disp_count, // count array from array_displacements struct
	    int* writer_count, // local dimensions from all writers; from offset_struct
	    uint64_t* reader_count, // the count field from reader's selector
	    char* from, 
	    char* to) 
{
    if(dim==ndims-1) {
        int* reader_count_copy = (int*)malloc(sizeof(int)*ndims);
        int i=0;
        for(i=0; i<ndims; i++){
            reader_count_copy[i]=reader_count[i];
        }
        int s = linearize_displ(disp_start, writer_count, ndims, data_size);
        int e = linearize_displ(disp_start, reader_count_copy, ndims, data_size);
	free(reader_count_copy);
        memcpy(to, from+s,  data_size*disp_count[ndims-1]);
    } else {
        int i;
        for(i=0; i<disp_count[dim]; i++) {
            int* disp_startcpy = malloc(sizeof(int)*ndims);
            memcpy(disp_startcpy, disp_start, sizeof(int)*ndims);
            disp_startcpy[dim] += i;
            copyoffsets(dim+1, 
			ndims, 
			data_size, 
			disp_start,
			disp_count, 
			writer_count, 
			reader_count, 
			from, 
			to);
        }
    }
}

/*
 * Will replace data handler once everything is figured out and working wrt conditions and the fm
 * get/set api.
 */
static int
raw_handler(CManager cm, void *vevent, int len, void *client_data, attr_list attrs)
{
    ADIOS_FILE *adiosfile = client_data;
    flexpath_file_data *fp = (flexpath_file_data*)adiosfile->fh;
    FMContext context = CMget_FMcontext(cm);
    void *base_data = FMheader_skip(context, vevent);
    FMFormat format = FMformat_from_ID(context, vevent);  
    
    FMStructDescList struct_list = 
	FMcopy_struct_list(format_list_of_FMFormat(format));
    FMField *f = struct_list[0].field_list;

    if(fp->num_vars == 0){
	int var_count = 0;
	int i=0;       
	while(f->field_name != NULL){           
	    flexpath_var_info * curr_var = new_flexpath_var_info(f->field_name, 
								 var_count, 
								 f->field_size);
	    curr_var->num_chunks = 1;
	    curr_var->chunks = malloc(sizeof(flexpath_var_chunk)*curr_var->num_chunks);
	    memset(curr_var->chunks, 0, sizeof(flexpath_var_chunk)*curr_var->num_chunks);

	    flexpath_var_info * temp = fp->var_list;	
	    curr_var->next = temp;
	    fp->var_list = curr_var;
	    //curr_var->type = adios_integer;
	    // because we're only doing scalars here, we know the dims is 0	
	    var_count++;
	    f++;
	}
	
	adiosfile->var_namelist = malloc(var_count * sizeof(char *));
	f = struct_list[0].field_list;  // f is top-level field list 
	
	while(f->field_name != NULL) {
	    adiosfile->var_namelist[i++] = strdup(f->field_name);
	    f++;
	}
	
	adiosfile->nvars = var_count;
	fp->num_vars = var_count;
    }

    int condition;
    int rank;          
    get_int_attr(attrs, attr_atom_from_string("fp_dst_condition"), &condition);   
    get_int_attr(attrs, attr_atom_from_string(FP_RANK_ATTR_NAME), &rank); 
    fp->current_format = format;
    f = struct_list[0].field_list;
    char * curr_offset = NULL;
    int i = 0, l = 0, j = 0;

    while(f->field_name){
    	//curr_offset = &buffer[f->field_offset];	
        char atom_name[200] = "";
    	flexpath_var_info * var = find_fp_var(fp->var_list, strdup(f->field_name));
	
    	if(!var){
    	    adios_error(err_file_open_error,
    			"file not opened correctly.  var does not match format.\n");
    	    return -1;
    	}
        strcat(atom_name, f->field_name);
        strcat(atom_name, "_");
        strcat(atom_name, FP_NDIMS_ATTR_NAME);
        int num_dims;
        int i;
        get_int_attr(attrs, attr_atom_from_string(strdup(atom_name)), &num_dims);
    	flexpath_var_chunk * curr_chunk = &var->chunks[0];
    	// scalar
        fp_log("SEL","inquiring num_dims %d of var %s\n", num_dims, f->field_name);
    	if(num_dims == 0){
	    
    	    curr_chunk->global_offsets = NULL;
    	    curr_chunk->global_bounds = NULL;
    	    curr_chunk->local_bounds = NULL;
    	    //curr_chunk->data = malloc(var->data_size);
    	    //void *tmpdata = malloc(var->data_size);
    	    var->chunks[0].data = get_FMfieldAddr_by_name(f, f->field_name, base_data);
    	    curr_chunk->has_data = 1;
    	    // else it's an array
    	}else{
            if(!var->sel){// var hasn't been scheduled yet.       
    		fp_log("SEL", "Variable has not yet been scheduled.  Cannot recieve data for it.\n");
    	    }
    	    else if(var->sel->type == ADIOS_SELECTION_WRITEBLOCK){
    		var->ndims = num_dims;
    		var->dims = malloc(sizeof(int)*num_dims);
    		if(var->was_scheduled == 1){
    		    var->array_size = var->data_size;
    		    for(i=0; i<num_dims; i++){
    			char *dim;
    			atom_name[0] ='\0';
    			strcat(atom_name, f->field_name);
    			strcat(atom_name, "_");
    			strcat(atom_name, FP_DIM_ATTR_NAME);
    			strcat(atom_name, "_");
    			char dim_num[10] = "";
    			sprintf(dim_num, "%d", i+1);
    			strcat(atom_name, dim_num);
    			get_string_attr(attrs, attr_atom_from_string(atom_name), &dim);
    			FMField * temp_f = find_field(dim, f);
    			if(!temp_f){
    			    adios_error(err_invalid_varname,
    					"Could not find fieldname: %s\n",
    					dim);
    			}
    			else{
    			    // since it's a dimension, field size should be int.
    			    int *temp_data = get_FMfieldAddr_by_name(temp_f,
    								     temp_f->field_name,
    								     base_data);
    			    //memcpy(temp_data, temp_offset, f->field_size);
    			    var->dims[i] = *temp_data;
    			    var->array_size = var->array_size * var->dims[i];
    			}
    		    }
    		    /* void* aptr8 = (void*)(*((unsigned long*)curr_offset)); */
    		    /* memcpy(var->chunks[0].data, aptr8, var->array_size); */
		    void *arrays_data  = get_FMPtrField_by_name(f, f->field_name, base_data, 1);
		    memcpy(var->chunks[0].data, arrays_data, var->array_size);
    		}

    	    }
    	    else if(var->sel->type == ADIOS_SELECTION_BOUNDINGBOX){
    		int i;
                global_var* gv = find_gbl_var(fp->gp->vars,
    					      var->varname,
    					      fp->gp->num_vars);
                int * writer_count = gv->offsets[0].local_dimensions;
                uint64_t * reader_count = var->sel->u.bb.count;
    		array_displacements * disp = find_displacement(var->displ,
    							       rank,
    							       var->num_displ);
		
    		//void *aptr8 = (void*)(*((unsigned long*)curr_offset));		
		void *aptr8 = get_FMPtrField_by_name(f, f->field_name, base_data, 1);
    		//double * temp = (double*)curr_offset;
		log_debug("first call to copyoffsets\n");
		log_debug("dim: %d\n", 0);
		log_debug("ndims: %d\n", disp->ndims);
		log_debug("data_size: %d\n", f->field_size);

		log_debug_cont("disp_start[]: ");
		int k;
		for(k=0; k<disp->ndims; k++){
		    log_debug_cont("%d ", disp->start[k]);
		}
		log_debug("\n");

		log_debug("disp_count[]: ");
		for(k=0; k<disp->ndims; k++){
		    log_debug_cont("%d ", disp->count[k]);
		}
		log_debug("\n");

		log_debug("writer_count[]: ");
		for(k=0; k<disp->ndims; k++){
		    log_debug_cont("%d ", writer_count[k]);
		}
		log_debug("\n");
		
		log_debug("reader_count[]: ");
		for(k=0; k<disp->ndims; k++){
		    log_debug_cont("%d ", (int)reader_count[k]);
		}
                copyoffsets(0,
    			    disp->ndims,
    			    f->field_size,
    			    disp->start,
    			    disp->count,
    			    writer_count,
    			    reader_count,
    			    (char*)aptr8,
    			    (char*)var->chunks[0].data);
    	    }
    	}
        j++;
        f++;
    }
    CMCondition_signal(fp_read_data->fp_cm, condition);
    //CMCondition_signal(fp_read_data->fp_cm, ackCondition);
    //ackCondition = CMCondition_get(fp_read_data->fp_cm, NULL);
    return 0; 
}

static int
format_handler(CManager cm, void *vevent, void *client_data, attr_list attrs) {
    Format_msg* msg = (Format_msg*)vevent;
    ADIOS_FILE *adiosfile = client_data;
    flexpath_file_data * fp = (flexpath_file_data*)adiosfile->fh;
    fp->arr = msg->format_id;
    fp->rep_id = msg->rep_id;
    fp->rep_id_len = msg->rep_id_len;
    fp->id_len = msg->id_len;
    
    CMCondition_signal(fp_read_data->fp_cm, msg->condition);
    //ackCondition = CMCondition_get(fp_read_data->fp_cm, NULL);
    return 0;
}

/*
 * Initializes flexpath read structures for a client read
 * - malloc space for global values
 * - store reference to MPI_Comm and get MPI information
 * - store reference to a new evpath connection manager instance
 */
int
adios_read_flexpath_init_method (MPI_Comm comm, PairStruct* params)
{    
    setenv("CMSelfFormats", "1", 1);
    fp_read_data = malloc(sizeof(flexpath_read_data));     
    if(!fp_read_data) {
        adios_error(err_no_memory, "Cannot allocate memory for flexpath.");
        return -1;
    }
    memset(fp_read_data, 0, sizeof(flexpath_read_data));
    
    fp_read_data->CM_TRANSPORT = attr_atom_from_string("CM_TRANSPORT");
    attr_list listen_list = NULL;
    char * transport = NULL;
    transport = getenv("CMTransport");

    // setup MPI stuffs
    fp_read_data->fp_comm = comm;
    MPI_Comm_size(fp_read_data->fp_comm, &(fp_read_data->fp_comm_size));
    MPI_Comm_rank(fp_read_data->fp_comm, &(fp_read_data->fp_comm_rank));

    // setup connection manager
    gen_pthread_init();
    fp_read_data->fp_cm = CManager_create();
    if(transport == NULL){
	if(CMlisten(fp_read_data->fp_cm) == 0) {
	    log_error( "Flexpath ERROR: unable to initialize connection manager.\n");
	}
    }else{
	listen_list = create_attr_list();
	add_attr(listen_list, fp_read_data->CM_TRANSPORT, Attr_String, 
		 (attr_value)strdup(transport));
	CMlisten_specific(fp_read_data->fp_cm, listen_list);
    }
    if(CMfork_comm_thread(fp_read_data->fp_cm)) {/*log_debug( "forked\n");*/}
    return 0;
}


void build_bridge(bridge_info* bridge) {
    attr_list contact_list = attr_list_from_string(bridge->contact);

    bridge->bridge_stone =
        EVcreate_bridge_action(fp_read_data->fp_cm,
            contact_list,
            (EVstone)bridge->their_num);

    bridge->flush_source =
        EVcreate_submit_handle(fp_read_data->fp_cm,
            bridge->bridge_stone,
            flush_format_list);

    bridge->var_source =
	EVcreate_submit_handle(fp_read_data->fp_cm,
	    bridge->bridge_stone,
	    var_format_list);

    bridge->op_source =
	EVcreate_submit_handle(fp_read_data->fp_cm,
	    bridge->bridge_stone,
	    op_format_list);

    bridge->created = 1;

}


ADIOS_FILE*
adios_read_flexpath_open_file(const char * fname, MPI_Comm comm)
{
    adios_error (err_operation_not_supported,
                 "FLEXPATH staging method does not support file mode for reading. "
                 "Use adios_read_open() to open a staged dataset.\n");
    return NULL;
}

/*
 * Still have work to do here.  
 * Change it so that we can support the timeouts and lock_modes.
 */
/*
 * Sets up local data structure for series of reads on an adios file
 * - create evpath graph and structures
 * -- create evpath control stone (outgoing)
 * -- create evpath data stone (incoming)
 * -- rank 0 dumps contact info to file
 * -- create connections using contact info from file
 */
ADIOS_FILE*
adios_read_flexpath_open(const char * fname,
				MPI_Comm comm,
                                enum ADIOS_LOCKMODE lock_mode,
				float timeout_sec)
{
    log_debug("\t\t FLEXPATH READER OPEN\n\n");
    ADIOS_FILE *adiosfile = malloc(sizeof(ADIOS_FILE));        
    if(!adiosfile){
	adios_error (err_no_memory, "Cannot allocate memory for file info.\n");
	return NULL;
    }    
    
    flexpath_file_data *fp = new_flexpath_file_data(fname);
    fp->ackCondition = CMCondition_get(fp_read_data->fp_cm, NULL);
	
    adios_errno = 0;
    fp->data_stone = EValloc_stone(fp_read_data->fp_cm);	
    fp->comm = comm;

    MPI_Comm_size(fp->comm, &(fp->size));
    MPI_Comm_rank(fp->comm, &(fp->rank));
    EVassoc_terminal_action(fp_read_data->fp_cm,
			    fp->data_stone,
			    op_format_list,
			    op_msg_handler,
			    adiosfile);
	
    EVassoc_terminal_action(fp_read_data->fp_cm,
			    fp->data_stone,
			    format_format_list,
			    format_handler,
			    adiosfile);

    EVassoc_terminal_action(fp_read_data->fp_cm,
			    fp->data_stone,
			    evgroup_format_list,
			    group_msg_handler,
			    adiosfile);
    EVassoc_raw_terminal_action(fp_read_data->fp_cm,
				fp->data_stone,
				raw_handler,
				(void*)adiosfile);

    char writer_ready_filename[200];
    char writer_info_filename[200];
    char reader_ready_filename[200];
    char reader_info_filename[200];
	
    sprintf(reader_ready_filename, "%s_%s", fname, READER_READY_FILE);
    sprintf(reader_info_filename, "%s_%s", fname, READER_CONTACT_FILE);
    sprintf(writer_ready_filename, "%s_%s", fname, WRITER_READY_FILE);
    sprintf(writer_info_filename, "%s_%s", fname, WRITER_CONTACT_FILE);
	
    char * string_list;
    char data_contact_info[50];
    string_list = attr_list_to_string(CMget_contact_list(fp_read_data->fp_cm));
    sprintf(&data_contact_info[0], "%d:%s", fp->data_stone, string_list);
    char * recvbuf;
    if(fp->rank == 0){	
	recvbuf = (char*)malloc(sizeof(char)*50*(fp->size));
    }

#ifndef _NOMPI
    MPI_Gather(data_contact_info, 50, MPI_CHAR, recvbuf,
	       50, MPI_CHAR, 0, fp->comm);
#endif

    if(fp->rank == 0){	
	// print our own contact information

	FILE * fp_out = fopen(reader_info_filename, "w");
	int i;
	if(!fp_out){	    
	    adios_error(err_file_open_error,
			"File for contact info could not be opened for writing.\n");
	    exit(1);
	}
	for(i=0; i<fp->size; i++) {
	    fprintf(fp_out,"%s\n", &recvbuf[i*50]);
	}
	fclose(fp_out);
	fp_out = fopen(reader_ready_filename, "w");
	fprintf(fp_out, "ready");
	fclose(fp_out);
	free(recvbuf);

	FILE * read_ready = fopen(reader_ready_filename, "w");
	fprintf(read_ready, "ready");
	fclose(read_ready);
    }
    MPI_Barrier(fp->comm);
    //may need to switch to rank 0 and mpi broadcast
    FILE * fp_in = fopen(writer_ready_filename,"r");
    while(!fp_in) {
	CMsleep(fp_read_data->fp_cm, 1);
	fp_in = fopen(writer_ready_filename, "r");
    }
    fclose(fp_in);
    fp_in = fopen(writer_info_filename, "r");
    while(!fp_in){
	CMsleep(fp_read_data->fp_cm, 1);
	fp_in = fopen(writer_info_filename, "r");
    }

    char in_contact[50] = "";
    fp->bridges = malloc(sizeof(bridge_info));
    int num_bridges = 0;
    int their_stone;

    // change to read all numbers, dont create stones, turn bridge array into linked list
    while(fscanf(fp_in, "%d:%s", &their_stone, in_contact) != EOF){	
	fp->bridges = realloc(fp->bridges,
					  sizeof(bridge_info) * (num_bridges+1));
	fp->bridges[num_bridges].their_num = their_stone;
	fp->bridges[num_bridges].contact = strdup(in_contact);
	fp->bridges[num_bridges].created = 0;
	fp->bridges[num_bridges].step = 0;
	fp->bridges[num_bridges].opened = 0;
	num_bridges++;
    }
    fclose(fp_in);
    //build_bridge(&fp->bridges[0]);

    fp->num_bridges = num_bridges;
    // clean up of writer's files
    MPI_Barrier(fp->comm);
    if(fp->rank == 0){
	unlink(writer_info_filename);
	unlink(writer_ready_filename);
    }	
    
    adiosfile->fh = (uint64_t)fp;
    adiosfile->current_step = 0;
    
    // determining from which writer to read
    // if there are less writers
    int writer_rank = fp->rank % num_bridges;
    /* if(num_bridges < fp->size){ */
    /* 	writer_rank = fp->size % num_bridges; */
    /* 	// otherwise */
    /* }else{ */
    /* 	writer_rank = fp->rank; */
    /* }     */
    build_bridge(&fp->bridges[writer_rank]);
    fp->writer_coordinator = writer_rank;
    op_msg init;
    init.step = 0;
    init.type = 3;
    init.process_id = fp->rank;
    init.file_name = "test";
    init.condition = CMCondition_get(fp_read_data->fp_cm, NULL);
    EVsubmit(fp->bridges[writer_rank].op_source, &init, NULL);
    CMCondition_wait(fp_read_data->fp_cm, init.condition); 
    /* Flush_msg msg; */
    /* msg.type = DATA; */
    /* msg.rank = fp->rank; */
    /* msg.condition = CMCondition_get(fp_read_data->fp_cm, NULL); */
    /* EVsubmit(fp->bridges[0].flush_source, &msg, NULL); */
    /* CMCondition_wait(fp_read_data->fp_cm, msg.condition); */
    adios_errno == err_no_error;
    log_debug("\t\tFLEXPATH READER LEAVING OPEN\n");
    return adiosfile;
}

int adios_read_flexpath_finalize_method ()
{
    return 0;
}

void adios_read_flexpath_release_step(ADIOS_FILE *adiosfile) {
    int i;
    log_debug("\t\tFLEXPATH READER RELEASE_STEP\n");
    flexpath_file_data *fp = (flexpath_file_data*)adiosfile->fh;
    for(i=0; i<fp->num_bridges; i++) {
        if(fp->bridges[i].created && !fp->bridges[i].opened) {
            op_msg open;
            open.step = adiosfile->current_step;
            open.type = OPEN_MSG;
            open.process_id = fp->rank;
            open.file_name = "test";
	    open.condition = CMCondition_get(fp_read_data->fp_cm, NULL);
            EVsubmit(fp->bridges[i].op_source, &open, NULL);
            CMCondition_wait(fp_read_data->fp_cm, open.condition);
	    fp->bridges[i].opened = 1;
        }
    }
}

int adios_read_flexpath_advance_step(ADIOS_FILE *adiosfile, int last, float timeout_sec) {
    flexpath_file_data *fp = (flexpath_file_data*)adiosfile->fh;
    log_debug("FLEXPATH ADVANCE_STEP CALLED!\n");
    MPI_Barrier(fp->comm);
    int i=0;
    for(i=0; i<fp->num_bridges; i++) {
        if(fp->bridges[i].created && fp->bridges[i].opened) {
            op_msg close;
            close.step = adiosfile->current_step;
            close.type = 0;
            close.process_id = fp->rank;
	    close.condition = -1;
            close.file_name = "test";
	    fp->bridges[i].opened = 0;
            EVsubmit(fp->bridges[i].op_source, &close, NULL);

            op_msg open;
            open.step = adiosfile->current_step +1;
            open.type = 1;
            open.process_id = fp->rank;
            open.file_name = "test";
	    open.condition = CMCondition_get(fp_read_data->fp_cm, NULL);
	    fp->bridges[i].opened = 1;
            EVsubmit(fp->bridges[i].op_source, &open, NULL);
            CMCondition_wait(fp_read_data->fp_cm, open.condition);
        }
    }
    adiosfile->current_step++;
    
   return 0;
}

int adios_read_flexpath_close(ADIOS_FILE * fp)
{
    flexpath_file_data * file = (flexpath_file_data*)fp->fh;
    int i;
    op_msg msg;
    msg.type=0;
    msg.file_name = strdup(file->file_name);
    msg.process_id = file->rank;
    msg.condition = CMCondition_get(fp_read_data->fp_cm, NULL);
    //send to each opened link
    for(i = 0; i<file->num_bridges; i++){
        if(file->bridges[i].created) {
            msg.step = file->bridges[i].step;
	    EVsubmit(file->bridges[i].op_source, &msg, NULL);
        }
    }
    /*
    start to cleanup.  Clean up var_lists for now, as the
    data has already been copied over to ADIOS_VARINFO structs
    that the user maintains a copy of. 
    */
    flexpath_var_info * v = file->var_list;
    while(v){        	
    	// free chunks; data has already been copied to user
    	int i;	
    	for(i = 0; i<v->num_chunks; i++){    		    
    	    flexpath_var_chunk * c = &v->chunks[i];	    
	    if(!c)
		log_error("FLEXPATH: %s This should not happen! line %d\n",__func__,__LINE__);
	    //free(c->data);    		
	    free(c->global_bounds);		
	    free(c->global_offsets);
	    free(c->local_bounds);
	}
    	v=v->next;
    }
    return 0;
}

ADIOS_FILE *adios_read_flexpath_fopen(const char *fname, MPI_Comm comm) {
   return 0;
}

int adios_read_flexpath_is_var_timed(const ADIOS_FILE* fp, int varid) { return 0; }

void adios_read_flexpath_get_groupinfo(const ADIOS_FILE *fp, int *ngroups, char ***group_namelist, int **nvars_per_group, int **nattrs_per_group) {}

int adios_read_flexpath_check_reads(const ADIOS_FILE* fp, ADIOS_VARCHUNK** chunk) { log_debug( "flexpath:adios function check reads\n"); return 0; }

int adios_read_flexpath_perform_reads(const ADIOS_FILE *adiosfile, int blocking)
{
    flexpath_file_data * fp = (flexpath_file_data*)adiosfile->fh;
    Flush_msg msg;
    msg.rank = fp->rank;
    msg.type = DATA;
    msg.condition = CMCondition_get(fp_read_data->fp_cm, NULL);
    int i;
    int num_sendees = fp->num_sendees;
    for(i = 0; i<num_sendees; i++)
    {
	int sendee = fp->sendees[i];
        fp_log("MSG","rank %d sending flush to %d\n", fp->rank, sendee);
	EVsubmit(fp->bridges[sendee].flush_source, &msg, NULL);
    }
    if(blocking){    
	CMCondition_wait(fp_read_data->fp_cm, msg.condition);
    }
    return 0;
}
int
adios_read_flexpath_inq_var_blockinfo(const ADIOS_FILE* fp,
				      ADIOS_VARINFO* varinfo)
{ /*log_debug( "flexpath:adios function inq var block info\n");*/ return 0; }
int
adios_read_flexpath_inq_var_stat(const ADIOS_FILE* fp,
				 ADIOS_VARINFO* varinfo,
				 int per_step_stat,
				 int per_block_stat)
{ /*log_debug( "flexpath:adios function inq var stat\n");*/ return 0; }
void adiosread_flexpath_release_step (ADIOS_FILE *fp);



array_displacements*
get_writer_displacements(int rank, const ADIOS_SELECTION * sel, global_var* gvar)
{
    int ndims = sel->u.bb.ndim;
    array_displacements * displ = (array_displacements*)malloc(sizeof(array_displacements));
    displ->writer_rank = rank;

    displ->start = (int*)malloc(sizeof(int) * ndims);
    displ->count = (int*)malloc(sizeof(int) * ndims);    
    displ->ndims = ndims;
    int * offsets = gvar->offsets[0].local_offsets;
    int * local_dims = gvar->offsets[0].local_dimensions;
    int pos = rank * gvar->offsets[0].offsets_per_rank;
    // malloc of ndims size;
    //for each dim
    int i;
    for(i=0; i<ndims; i++){	
	if(sel->u.bb.start[i] >= offsets[pos+i]){
	    int start = sel->u.bb.start[i] - offsets[pos+i];
	    displ->start[i] = start;
	}
	if((sel->u.bb.start[i] + sel->u.bb.count[i] - 1) <= (offsets[pos+i] + local_dims[pos+i] - 1)){	   
	    int count = ((sel->u.bb.start[i] + sel->u.bb.count[i] - 1) - offsets[pos+i]) - displ->start[i] + 1;
	    displ->count[i] = count;
	    
	}else{
	    int count = (local_dims[pos+i] - 1) - displ->start[i] + 1;
	    displ->count[i] = count;
	}	
    }
    return displ;
}

int
need_writer(flexpath_file_data *fp, int j, const ADIOS_SELECTION* sel, evgroup_ptr gp, char* varname) {    
    /* if(!file_data_list->gp){ */
    /* 	CMCondition_wait(fp_read_data->fp_cm, ackCondition); */
    /* } */

    while(!fp->gp)
	CMsleep(fp_read_data->fp_cm, 1);
    //select var from group
    global_var * gvar = find_gbl_var(gp->vars, varname, gp->num_vars);

    //for each dimension
    int i=0;
    offset_struct var_offsets = gvar->offsets[0];
    for(i=0; i< var_offsets.offsets_per_rank; i++){
        //select sel offsets
        int sel_offset = sel->u.bb.start[i];
        //grab sel dimensions(size)
        int sel_size = sel->u.bb.count[i];        


        //select rank offsets
        int rank_offset = var_offsets.local_offsets[j*var_offsets.offsets_per_rank+i];
        //grab rank dimencsions(size)
        int rank_size =var_offsets.local_dimensions[j*var_offsets.offsets_per_rank+i];        

        //if rank offset < selector offset and rank offset +size-1 > selector offset
	
        if((rank_offset <= sel_offset) && (rank_offset + rank_size - 1 >=sel_offset)) {
	     log_debug("matched overlap type 1\n");
        }
        //if rank offset < selector offset + selector size -1 and rank offset+size-1 > selector offset +selector size -1
        else if((rank_offset <= sel_offset + sel_size - 1) && \
		(rank_offset+rank_size-1>=sel_offset+sel_size-1)) {
            log_debug("matched overlap type 2\n");
        } else {
            log_debug("overlap not present\n\n");
            return 0;
        }
    }
    log_debug("overlap detected\n\n");
    return 1;
}


int adios_read_flexpath_schedule_read_byid(const ADIOS_FILE * adiosfile,
					   const ADIOS_SELECTION * sel,
					   int varid,
					   int from_steps,
					   int nsteps,
					   void * data)
{   
    log_debug("\t\tFLEXPATH READER CALLING SCHEDULE_READ\n");
    flexpath_file_data * fp = (flexpath_file_data*)adiosfile->fh;
    flexpath_var_info * v = fp->var_list;
    while(v){
        if(v->id == varid)
        	break;
        else
    	v=v->next;
    }
    if(!v){
        adios_error(err_invalid_varid,
    		"Invalid variable id: %d\n",
    		varid);
        return err_invalid_varid;
    }    
    //store the user allocated buffer.
    flexpath_var_chunk * chunk = &v->chunks[0];  
    chunk->data = data;
    v->was_scheduled = 1;
    if(nsteps != 1){
	adios_error (err_invalid_timestep,
                     "Only one step can be read from a stream at a time. "
                     "You requested % steps in adios_schedule_read()\n", nsteps);
        return err_invalid_timestep;
    }
    v->sel = sel;
    switch(sel->type)
    {
    case ADIOS_SELECTION_WRITEBLOCK:
    {
	int writer_index = sel->u.block.index;
	if(writer_index > fp->num_bridges){
	    adios_error(err_out_of_bound,
			"No process exists on the writer side matching the index.\n");
	}


	//log_debug( "rank %d sending var message\n", fd->rank);
	//send to what is specified in selector
        int i = 0;
        int found = 0;
        for(i=0; i<fp->num_sendees; i++) {
            if(fp->sendees[i]==writer_index) {
                found=1;
                break;
            }
        }
        if(!found) {
            fp->num_sendees+=1;
            fp->sendees=realloc(fp->sendees, fp->num_sendees*sizeof(int));
            fp->sendees[fp->num_sendees-1] = writer_index;
        }
        if(!fp->bridges[writer_index].created) {
            build_bridge(&(fp->bridges[writer_index]));
	}
	if(!fp->bridges[writer_index].opened){
	    fp->bridges[writer_index].opened = 1;
            op_msg open_msg;
            open_msg.process_id = fp->rank;
            open_msg.file_name = fp->file_name;
            open_msg.type = 1;
            open_msg.step = 0;
	    open_msg.condition = CMCondition_get(fp_read_data->fp_cm, NULL);
            EVsubmit(fp->bridges[writer_index].op_source, &open_msg, NULL);
            CMCondition_wait(fp_read_data->fp_cm, open_msg.condition);
	}
	Var_msg var;
	var.rank = fp->rank;
	var.var_name = strdup(v->varname);
	EVsubmit(fp->bridges[writer_index].var_source, &var, NULL);    
	break;
    }
    case ADIOS_SELECTION_BOUNDINGBOX:
    {
        fp_log("BOUNDING", "bounding box scheduled read\n");
	if(!fp->gp){
	    Flush_msg msg;
	    msg.type = EVGROUP;
	    msg.rank = fp->rank;
	    msg.condition = CMCondition_get(fp_read_data->fp_cm, NULL);
	    // maybe check to see if the bridge is create first.
	    EVsubmit(fp->bridges[fp->writer_coordinator].flush_source, &msg, NULL);
	    CMCondition_wait(fp_read_data->fp_cm, msg.condition);
	}
        int j=0;
	int need_count = 0;
	array_displacements * all_disp = NULL;	
        for(j=0; j<fp->size; j++) {
            fp_log("BOUNDING", "checking writer %d\n", j);
            int reader=0;	    	    
            if(need_writer(fp, j, sel, fp->gp, v->varname)==1){
                fp_log("BOUNDING", "yes it's neededi\n");		
		need_count++;
                reader = j;
		global_var * gvar = find_gbl_var(fp->gp->vars, v->varname, fp->gp->num_vars);
		array_displacements * displ = get_writer_displacements(j, sel, gvar);
		all_disp = realloc(all_disp, sizeof(array_displacements)*need_count);
		all_disp[need_count-1] = *displ;
            } else {
                fp_log("BOUNDING", "no it's not\n");		
                continue;
            }
            int i = 0;
            int found = 0;
            fp_log("BOUNDING", "remember who to read from\n");
            for(i=0; i<fp->num_sendees; i++) {
                if(fp->sendees[i]==reader) {
                    found=1;
                    break;
                }
            }
            if(!found) {
                fp->num_sendees+=1;
                fp->sendees=realloc(fp->sendees, fp->num_sendees*sizeof(int));
                fp->sendees[fp->num_sendees-1] = reader;
            }
            if(!fp->bridges[reader].created) {
                build_bridge(&(fp->bridges[reader]));
	    }
	    if(!fp->bridges[reader].opened){
		op_msg open_msg;
		open_msg.process_id = fp->rank;
		open_msg.file_name = "hey";
		open_msg.type = 1;
		open_msg.step = adiosfile->current_step;
		fp->bridges[reader].opened = 1;
		open_msg.condition = CMCondition_get(fp_read_data->fp_cm, NULL);
		EVsubmit(fp->bridges[reader].op_source, &open_msg, NULL);
		CMCondition_wait(fp_read_data->fp_cm, open_msg.condition);
	    }
	
	    Var_msg var;
	    var.rank = fp->rank;
	    var.var_name = strdup(v->varname);
	    log_debug("1 sending %s from %d to %p aka %d\n", 
		 var.var_name, var.rank, fp->bridges[reader].var_source, reader);
	    EVsubmit(fp->bridges[reader].var_source, &var, NULL);            
	}
	v->displ = all_disp;
	v->num_displ = need_count;
        break;
    }
    case ADIOS_SELECTION_AUTO:
    {
	adios_error(err_operation_not_supported,
		    "ADIOS_SELECTION_AUTO not yet supported by flexpath.");
	break;
    }
    case ADIOS_SELECTION_POINTS:
    {
	adios_error(err_operation_not_supported,
		    "ADIOS_SELECTION_POINTS not yet supported by flexpath.");
	break;
    }
    }
    log_debug("\t\tFLEXPATH READER LEAVING SCHEDULE_READ\n\n");
    return 0;
}

int adios_read_flexpath_schedule_read(const ADIOS_FILE *adiosfile,
			const ADIOS_SELECTION * sel,
			const char * varname,
			int from_steps,
			int nsteps,
			void * data)
{
    return 0;
}

int adios_read_flexpath_fclose(ADIOS_FILE *adiosfile)
{
    //log_debug( "debug: adios_read_flexpath_fclose\n");
    return 0;
}

int * adios_read_flexpath_gopen (ADIOS_FILE *adiosfile, const char *grpname)
{
    //log_debug( "debug: adios_read_flexpath_gopen\n");
    return NULL;
}

int * adios_read_flexpath_gopen_byid (ADIOS_FILE *adiosfile, int grpid)
{
    //log_debug( "debug: adios_read_flexpath_gopen_byid\n");
    return NULL;
}

int adios_read_flexpath_gclose (int *gp)
{
    log_debug( "adios_read_flexpath_gclose\n");
    adios_errno = 0;
    int i;
//    free_namelist ((gp->var_namelist),gp->vars_count);
    free(gp);
    return 0;

}

int adios_read_flexpath_get_attr (int *gp, const char *attrname,
                                 enum ADIOS_DATATYPES *type,
                                 int *size, void **data)
{
    //log_debug( "debug: adios_read_flexpath_get_attr\n");
    // TODO: borrowed from dimes
    adios_error(err_invalid_read_method, "adios_read_flexpath_get_attr is not implemented.");
    *size = 0;
    *type = adios_unknown;
    *data = 0;
    return adios_errno;
}

int adios_read_flexpath_get_attr_byid (const ADIOS_FILE *adiosfile, int attrid,
                                      enum ADIOS_DATATYPES *type,
                                      int *size, void **data)
{
//    log_debug( "debug: adios_read_flexpath_get_attr_byid\n");
    // TODO: borrowed from dimes
    adios_error(err_invalid_read_method, "adios_read_flexpath_get_attr_byid is not implemented.");
    *size = 0;
    *type = adios_unknown;
    *data = 0;
    return adios_errno;
}

ADIOS_VARINFO* adios_read_flexpath_inq_var(const ADIOS_FILE * adiosfile, const char* varname)
{
    log_debug("\t\tFLEXPATH READER CALLS INQ_VAR\n\n");
    flexpath_file_data *fp = (flexpath_file_data*)adiosfile->fh;
    ADIOS_VARINFO* v = malloc(sizeof(ADIOS_VARINFO));
    if(!v) {
        adios_error(err_no_memory, "Cannot allocate buffer in adios_read_datatap_inq_var()");
	log_debug("\t\tFLEXPATH INQ_VAR_ERROR\n\n");
        return NULL;
    }
    memset(v, 0, sizeof(ADIOS_VARINFO));
    
    flexpath_var_info *current_var = find_fp_var(fp->var_list, varname);
    if(current_var) {
	v = convert_file_info(current_var, v, varname, adiosfile);
	log_debug("\t\tFLEXPATH READER LEAVING INQ_VAR\n");
	return v;
    }
    else {
	log_debug("\t\t%s OOOPS, line %d\n", __func__, __LINE__);
        adios_error(err_invalid_varname, "Cannot find var %s\n", varname);
        return NULL;
    }
}

ADIOS_VARINFO * adios_read_flexpath_inq_var_byid (const ADIOS_FILE * adiosfile, int varid)
{
    flexpath_file_data *fp = (flexpath_file_data*)adiosfile->fh;
    if(varid >= 0 && varid < adiosfile->nvars) {
        return adios_read_flexpath_inq_var(adiosfile, adiosfile->var_namelist[varid]);
    }
    else {
        adios_error(err_invalid_varid, "FLEXPATH method: Cannot find var %d\n", varid);
        return NULL;
    }
}

ADIOS_VARINFO* 
convert_file_info(flexpath_var_info * current_var,
				 ADIOS_VARINFO * v, const char* varname,
				 const ADIOS_FILE *adiosfile)
{
    int i;
    flexpath_file_data *fp = (flexpath_file_data*)adiosfile->fh;
    current_var->type = v->type;
    for(i = 0; i < adiosfile->nvars; i ++) {
	if(!strcmp(adiosfile->var_namelist[i], varname)) {
	    v->varid = i; // TODO: this may not be cmpatible with BP
	    break;
	}
    }
    v->type = current_var->type;
    v->ndim = current_var->ndims;
    //v->timedim = current_var->time_dim;
    if(v->ndim == 0){    
	//int value_size = common_read_type_size(v->type, current_var->chunks->data);
	int value_size = current_var->data_size;
	v->value = malloc(value_size);
	if(!v->value) {
	    adios_error(err_no_memory, "Cannot allocate buffer in adios_read_datatap_inq_var()");
	    return NULL;
	}
	flexpath_var_chunk * chunk = &current_var->chunks[0];
	memcpy(v->value, chunk->data, value_size);
    }else{ // arrays
	v->dims = (uint64_t *) malloc(v->ndim * sizeof(uint64_t));
	if(!v->dims) {
	    adios_error(err_no_memory, "Cannot allocate buffer in adios_read_datatap_inq_var()");
	    return NULL;
	}
	int k;
	for(k = 0; k < v->ndim; k ++) {
	    //v->dims[k] = ds->pgs[i].vars[j].global_bounds[k];
	    v->dims[k] = current_var->chunks->global_bounds[k];
	}
    }
    return v;
}


void adios_read_flexpath_free_varinfo (ADIOS_VARINFO *adiosvar)
{
    //log_debug( "debug: adios_read_flexpath_free_varinfo\n");
    return;
}

int64_t adios_read_flexpath_read_var (int *gp, const char *varname,
                                     const uint64_t *start, const uint64_t *count,
                                     void *data)
{
    //log_debug( "debug: adios_read_flexpath_read_var\n");
    return (int64_t)0;
}

int64_t adios_read_flexpath_read_var_byid (int *gp, int varid,
                                          const uint64_t *start,
                                          const uint64_t *count,
                                          void *data)
{
    //log_debug( "debug: adios_read_flexpath_read_var_byid\n");
    return (int64_t)0;
}

void adios_read_flexpath_reset_dimension_order (const ADIOS_FILE *adiosfile, int is_fortran)
{
    //log_debug( "debug: adios_read_flexpath_reset_dimension_order\n");
    adios_error(err_invalid_read_method, "adios_read_flexpath_reset_dimension_order is not implemented.");
}
