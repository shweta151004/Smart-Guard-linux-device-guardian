#include <stdio.h>
#include <stdlib.h>
#include <microhttpd.h>

int main(void)
{
    printf(
        "libmicrohttpd version: %s\n",
        MHD_get_version()
    );

    if (
        MHD_is_feature_supported(
            MHD_FEATURE_SSL
        ) == MHD_YES
    )
    {
        printf("HTTPS support: available\n");
        return EXIT_SUCCESS;
    }

    printf("HTTPS support: unavailable\n");
    return EXIT_FAILURE;
}
