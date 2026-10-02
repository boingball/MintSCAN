#include <assert.h>
#include <stdio.h>
#include "escl_format.h"

static void check(const char *xml, const char *mime, int expected, int known_expected)
{
    int known = -1;
    assert(ms_escl_format_supported(xml, mime, &known) == expected);
    assert(known == known_expected);
}

int main(void)
{
    const char *only_jpeg_pdf =
        "<scan:DocumentFormats>"
        "<pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat>"
        "<pwg:DocumentFormat>application/pdf</pwg:DocumentFormat>"
        "</scan:DocumentFormats>";
    const char *png_ext =
        "<pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat>"
        "<scan:DocumentFormatExt> image/png \n</scan:DocumentFormatExt>";
    const char *jpeg_ext =
        "<pwg:DocumentFormat>image/png</pwg:DocumentFormat>"
        "<scan:DocumentFormatExt>image/jpeg</scan:DocumentFormatExt>";

    check(only_jpeg_pdf, "image/jpeg", 1, 1);
    check(only_jpeg_pdf, "application/pdf", 1, 1);
    check(only_jpeg_pdf, "image/png", 0, 1);
    check(png_ext, "image/png", 1, 1);
    check("<scan:DocumentFormats/>", "image/png", 0, 0);
    check("", "image/png", 0, 0);
    check("<pwg:DocumentFormat>image/png2</pwg:DocumentFormat>", "image/png", 0, 1);
    check("<pwg:DocumentFormat>image/png;profile=x</pwg:DocumentFormat>", "image/png", 0, 1);
    assert(ms_escl_has_format_value(png_ext, "scan:DocumentFormatExt", "image/png"));
    assert(!ms_escl_has_format_value(jpeg_ext, "scan:DocumentFormatExt", "image/png"));
    puts("eSCL document-format capability tests passed");
    return 0;
}
