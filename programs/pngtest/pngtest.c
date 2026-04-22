/* Minimal test to diagnose png_create_read_struct_2 failure */
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

static png_voidp my_malloc(png_structp o, png_size_t size) {
    void *p = malloc(size);
    fprintf(stderr, "  png_malloc_cb(%zu) = %p\n", (size_t)size, p);
    return p;
}

static void my_free(png_structp o, png_voidp x) {
    fprintf(stderr, "  png_free_cb(%p)\n", x);
    free(x);
}

static void my_error(png_structp png_ptr, const char *msg) {
    fprintf(stderr, "  PNG ERROR: %s\n", msg);
    /* Don't longjmp here for test - just print */
}

static void my_warning(png_structp png_ptr, const char *msg) {
    fprintf(stderr, "  PNG WARNING: %s\n", msg);
}

int main(void) {
    fprintf(stderr, "=== PNG create_read_struct test ===\n");
    fprintf(stderr, "PNG_LIBPNG_VER_STRING = '%s'\n", PNG_LIBPNG_VER_STRING);
    fprintf(stderr, "sizeof(png_structp) = %zu bytes\n", sizeof(png_structp));

    /* Test 1: plain png_create_read_struct */
    fprintf(stderr, "\nTest 1: png_create_read_struct (default allocator)\n");
    png_structp png1 = png_create_read_struct(
        PNG_LIBPNG_VER_STRING, NULL, my_error, my_warning);
    if (png1) {
        fprintf(stderr, "  SUCCESS: %p\n", (void*)png1);
        png_destroy_read_struct(&png1, NULL, NULL);
    } else {
        fprintf(stderr, "  FAILED: returned NULL\n");
    }

    /* Test 2: png_create_read_struct_2 with custom allocator */
    fprintf(stderr, "\nTest 2: png_create_read_struct_2 (custom allocator)\n");
    png_structp png2 = png_create_read_struct_2(
        PNG_LIBPNG_VER_STRING, NULL, my_error, my_warning,
        NULL, my_malloc, my_free);
    if (png2) {
        fprintf(stderr, "  SUCCESS: %p\n", (void*)png2);
        png_infop info = png_create_info_struct(png2);
        if (info)
            fprintf(stderr, "  info_struct: %p (OK)\n", (void*)info);
        else
            fprintf(stderr, "  info_struct: NULL (FAILED)\n");
        png_destroy_read_struct(&png2, &info, NULL);
    } else {
        fprintf(stderr, "  FAILED: returned NULL\n");
    }

    /* Test 3: basic setjmp test */
    fprintf(stderr, "\nTest 3: setjmp test\n");
    jmp_buf jb;
    int ret = setjmp(jb);
    fprintf(stderr, "  setjmp returned %d (expect 0 on first call)\n", ret);
    if (ret == 0) {
        fprintf(stderr, "  setjmp OK, testing longjmp...\n");
        longjmp(jb, 42);
    } else {
        fprintf(stderr, "  longjmp returned %d (expect 42)\n", ret);
    }

    /* Test 4: test g_try_malloc equivalent */
    fprintf(stderr, "\nTest 4: malloc test\n");
    void *p = malloc(2048);
    fprintf(stderr, "  malloc(2048) = %p\n", p);
    free(p);

    /* Test 5: decode a minimal valid PNG from memory */
    fprintf(stderr, "\nTest 5: Decode minimal 1x1 PNG\n");
    /* This is a valid 1x1 red pixel PNG */
    static const unsigned char mini_png[] = {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,
        0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x02,0x00,0x00,
        0x00,0x90,0x77,0x53,0xde,0x00,0x00,0x00,0x0c,0x49,0x44,0x41,0x54,0x08,
        0xd7,0x63,0xf8,0xcf,0xc0,0x00,0x00,0x00,0x03,0x00,0x01,0x36,0x28,0x19,
        0x00,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
    };
    png_structp png5 = png_create_read_struct_2(
        PNG_LIBPNG_VER_STRING, NULL, my_error, my_warning,
        NULL, my_malloc, my_free);
    if (png5) {
        png_infop info5 = png_create_info_struct(png5);
        if (info5) {
            if (!setjmp(png_jmpbuf(png5))) {
                png_set_read_fn(png5, NULL, NULL);
                fprintf(stderr, "  read struct + info OK\n");
            }
        }
        png_destroy_read_struct(&png5, &info5, NULL);
    } else {
        fprintf(stderr, "  FAILED: png_create_read_struct_2 returned NULL\n");
    }

    fprintf(stderr, "\n=== Tests complete ===\n");
    return 0;
}
