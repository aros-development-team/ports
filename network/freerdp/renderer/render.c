/* AGPL-3.0-or-later: standalone MuPDF page renderer for ADT Remote Desktop. */
#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "render-native.h"
#include "render-spool.h"
extern fz_document_handler pdf_document_handler, xps_document_handler;
#define MAX_MEMORY (256U*1024U*1024U)
#define MAX_FILE (32L*1024L*1024L)
#define DPI 150
struct budget { size_t used, peak; };
union allocation { size_t size; long double align; void *pointer; };
static void *limited_malloc(void *arg,size_t size)
{
    struct budget *b=arg;
    if (size>MAX_MEMORY-sizeof(union allocation) || size+sizeof(union allocation)>MAX_MEMORY-b->used) return NULL;
    size+=sizeof(union allocation); union allocation *h=malloc(size); if (!h) return NULL;
    h->size=size; b->used+=size; if (b->used>b->peak) b->peak=b->used; return h+1;
}
static void limited_free(void *arg,void *ptr)
{
    if (!ptr) return;
    struct budget *b=arg; union allocation *h=(union allocation *)ptr-1;
    b->used-=h->size; free(h);
}
static void *limited_realloc(void *arg,void *ptr,size_t size)
{
    if (!ptr) return limited_malloc(arg,size);
    if (!size) { limited_free(arg,ptr); return NULL; }
    struct budget *b=arg; union allocation *h=(union allocation *)ptr-1;
    if (size>MAX_MEMORY-sizeof(*h)) return NULL;
    size+=sizeof(*h); size_t old=h->size;
    if (size>old && size-old>MAX_MEMORY-b->used) return NULL;
    union allocation *next=realloc(h,size); if (!next) return NULL;
    next->size=size; b->used=b->used-old+size; if (b->used>b->peak) b->peak=b->used;
    return next+1;
}
static int render_main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    if (argc<3 || (strcmp(argv[1],"--print") && strcmp(argv[1],"--hash") && strcmp(argv[1],"--ppm")) ||
        (!strcmp(argv[1],"--ppm") && argc!=4)) {
        puts("Usage: adt-rdp-render --print|--hash document | --ppm document output-prefix"); return 20;
    }
    FILE *input=fopen(argv[2],"rb"); if (!input) return 20;
    unsigned char magic[5]={0}; size_t got=fread(magic,1,sizeof(magic),input);
    int seek_ok=fseek(input,0,SEEK_END)==0; long length=seek_ok ? ftell(input) : -1; fclose(input);
    if (got<5 || length<1 || length>MAX_FILE || (memcmp(magic,"%PDF-",5) && memcmp(magic,"PK\003\004",4))) {
        fputs("Unsupported or oversized document\n",stderr); return 20;
    }
    struct budget budget={0}; fz_alloc_context alloc={&budget,limited_malloc,limited_realloc,limited_free};
    fz_context *ctx=fz_new_context(&alloc,NULL,32U*1024U*1024U); if (!ctx) return 20;
    fz_stream *stream=NULL; fz_document *doc=NULL; fz_page *page=NULL; fz_pixmap *pix=NULL;
    struct render_printer *printer=NULL; int result=20; uint64_t total_pixels=0;
    fz_var(stream); fz_var(doc); fz_var(page); fz_var(pix); fz_var(printer); fz_var(result);
    fz_try(ctx) {
        fz_register_document_handler(ctx,&pdf_document_handler);
        fz_register_document_handler(ctx,&xps_document_handler);
        stream=fz_open_file(ctx,argv[2]);
        doc=fz_open_document_with_stream(ctx,!memcmp(magic,"%PDF-",5) ? "application/pdf" : "application/vnd.ms-xpsdocument",stream);
        if (fz_needs_password(ctx,doc)) fz_throw(ctx,FZ_ERROR_UNSUPPORTED,"Encrypted print job requires a password");
        int count=fz_count_pages(ctx,doc);
        if (count<1 || count>100) fz_throw(ctx,FZ_ERROR_LIMIT,"Page count exceeds print limit");
        if (!strcmp(argv[1],"--print")) {
            printer=render_printer_open();
            if (!printer) fz_throw(ctx,FZ_ERROR_SYSTEM,"Cannot open configured printer.device");
        }
        for (int number=0;number<count;number++) {
            page=fz_load_page(ctx,doc,number);
            fz_matrix matrix=fz_scale(DPI/72.0f,DPI/72.0f);
            fz_rect bounds=fz_transform_rect(fz_bound_page(ctx,page),matrix);
            if (!isfinite(bounds.x0) || !isfinite(bounds.y0) || !isfinite(bounds.x1) || !isfinite(bounds.y1) ||
                bounds.x1-bounds.x0<1 || bounds.y1-bounds.y0<1 || bounds.x1-bounds.x0>6000 || bounds.y1-bounds.y0>6000)
                fz_throw(ctx,FZ_ERROR_LIMIT,"Page dimensions exceed print limit");
            fz_irect box=fz_round_rect(bounds);
            uint64_t pixels=(uint64_t)(box.x1-box.x0)*(uint64_t)(box.y1-box.y0);
            total_pixels+=pixels;
            if (pixels>16000000 || total_pixels>100000000) fz_throw(ctx,FZ_ERROR_LIMIT,"Raster budget exceeded");
            pix=fz_new_pixmap_from_page(ctx,page,matrix,fz_device_rgb(ctx),0);
            int width=fz_pixmap_width(ctx,pix), height=fz_pixmap_height(ctx,pix), stride=fz_pixmap_stride(ctx,pix);
            unsigned char *samples=fz_pixmap_samples(ctx,pix);
            if (fz_pixmap_components(ctx,pix)!=3) fz_throw(ctx,FZ_ERROR_FORMAT,"Expected RGB raster");
            fz_sha256 hash; unsigned char digest[32]; fz_sha256_init(&hash);
            for (int y=0;y<height;y++) fz_sha256_update(&hash,samples+(size_t)y*stride,(size_t)width*3);
            fz_sha256_final(&hash,digest);
            printf("RENDER_PAGE=%d SIZE=%dx%d RGB_SHA256=",number+1,width,height);
            for (unsigned i=0;i<32;i++) printf("%02x",digest[i]);
            putchar('\n');
            if (!strcmp(argv[1],"--ppm")) {
                char path[1024];
                if (snprintf(path,sizeof(path),"%s-%03d.ppm",argv[3],number+1)>=(int)sizeof(path)) fz_throw(ctx,FZ_ERROR_LIMIT,"Output path too long");
                fz_save_pixmap_as_pnm(ctx,pix,path);
            }
            if (printer && !render_printer_page(printer,samples,width,height,stride,DPI)) fz_throw(ctx,FZ_ERROR_SYSTEM,"printer.device page failed");
            fz_drop_pixmap(ctx,pix); pix=NULL; fz_drop_page(ctx,page); page=NULL;
        }
        printf("RENDER_COMPLETE=%d PEAK_BYTES=%zu\n",count,budget.peak); result=0;
    }
    fz_always(ctx) {
        render_printer_close(printer); fz_drop_pixmap(ctx,pix); fz_drop_page(ctx,page); fz_drop_document(ctx,doc); fz_drop_stream(ctx,stream);
    }
    fz_catch(ctx) { fprintf(stderr,"RENDER_ERROR=%s\n",fz_caught_message(ctx)); fz_ignore_error(ctx); }
    fz_drop_context(ctx); return result;
}

int main(int argc,char **argv)
{
    int spool=argc==3 && !strcmp(argv[1],"--spool");
    if (spool) argv[1]="--print";
    int result=render_main(argc,argv);
    if (spool) {
        render_spool_complete(argv[2],result);
        if (result) render_printer_error(argv[2]);
    }
    return result;
}
