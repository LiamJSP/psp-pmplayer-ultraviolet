#include <stddef.h>
#ifndef png_infopp_NULL
#define png_infopp_NULL NULL
#endif
#ifndef int_p_NULL
#define int_p_NULL NULL
#endif
#ifndef png_bytep_NULL
#define png_bytep_NULL NULL
#endif
#ifndef png_set_gray_1_2_4_to_8
#define png_set_gray_1_2_4_to_8 png_set_expand_gray_1_2_4_to_8
#endif
#include <stdlib.h>
#include <setjmp.h>
#include <malloc.h>
#include <string.h>
#include <png.h>
#include "mem64.h"
#include "imagefile.h"

static void user_warning_fn(png_structp png_ptr, png_const_charp warning_msg){
	(void)png_ptr;
	(void)warning_msg;
}

Image* loadPNGImage(const char* filename){
    png_structp png_ptr;
    png_infop info_ptr;
    png_uint_32 width, height;
    int bit_depth, color_type, interlace_type, pass, passes;
    png_uint_32 y;
    /* Value must survive libpng's longjmp after a truncated/corrupt row. */
    Image * volatile image = NULL;
    FILE *fp;

    if (!filename || (fp = fopen(filename, "rb")) == NULL) return NULL;
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png_ptr == NULL) { fclose(fp); return NULL; }
    png_set_error_fn(png_ptr, NULL, NULL, user_warning_fn);
    info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        fclose(fp); return NULL;
    }
    if (setjmp(png_jmpbuf(png_ptr))) {
        freeImage(image);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp); return NULL;
    }
    png_init_io(png_ptr, fp);
    png_read_info(png_ptr, info_ptr);
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth,
                 &color_type, &interlace_type, NULL, NULL);
    if (!width || !height || width > PPA_IMAGE_MAX_DIMENSION ||
        height > PPA_IMAGE_MAX_DIMENSION)
        png_error(png_ptr, "PPA UI image exceeds bounded texture dimensions");

    png_set_strip_16(png_ptr);
    png_set_packing(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);
    png_set_filler(png_ptr, 0xff, PNG_FILLER_AFTER);
    passes = png_set_interlace_handling(png_ptr);
    png_read_update_info(png_ptr, info_ptr);
    if (png_get_rowbytes(png_ptr, info_ptr) != (png_size_t)width * sizeof(Color))
        png_error(png_ptr, "PPA UI image could not be normalized to RGBA");

    image = createImage((int)width, (int)height);
    if (image == NULL) png_error(png_ptr, "PPA UI image allocation failed");
    /* Retain each row across Adam7 passes directly in its owned image. This
     * avoids the old temporary row buffer/copy and uninitialized grayscale
     * bytes while also allowing error cleanup to release the image. */
    for (pass = 0; pass < passes; ++pass)
        for (y = 0; y < height; ++y)
            png_read_row(png_ptr, (png_bytep)(image->data + y * image->textureWidth), NULL);
    png_read_end(png_ptr, info_ptr);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);
    return image;
}
