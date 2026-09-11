#include "samosa_html.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const unsigned char html[] =
        "\xef\xbb\xbf  <!-- leading -->\n<!DOCTYPE HTML><html><head>"
        "<title>Quarterly &amp; Notes</title><meta name='x' content='y'>HEAD_POISON"
        "<style>.poison{content:'STYLE_POISON'}</style></head><body>"
        "<h1>Results</h1><p>Revenue is &#x20ac;42 &mdash; verified.</p>"
        "<ul><li>Alpha</li><li>Beta &copy; 2026</li></ul>"
        "<table><tr><th>Key</th><th>Value</th></tr>"
        "<tr><td>late</td><td>HTML_LATE_SENTINEL</td></tr></table>"
        "<script>const secret = 'SCRIPT_POISON';</script>"
        "<template>TEMPLATE_POISON</template><svg><text>SVG_POISON</text></svg>"
        "<iframe>IFRAME_POISON</iframe><object>OBJECT_POISON</object>"
        "</body></html>";
    SamosaHtmlResult result;
    const char *error = NULL;

    assert(samosa_html_sniff(html, sizeof(html) - 1));
    assert(samosa_html_text_valid(html, sizeof(html) - 1, &error));
    assert(samosa_html_extract(html, sizeof(html) - 1, 64 * 1024,
                               &result, &error));
    assert(!strcmp(result.title, "Quarterly & Notes"));
    assert(strstr(result.text, "Results"));
    assert(strstr(result.text, "Revenue is \xe2\x82\xac" "42 \xe2\x80\x94 verified."));
    assert(strstr(result.text, "- Alpha"));
    assert(strstr(result.text, "Beta \xc2\xa9 2026"));
    assert(strstr(result.text, "HTML_LATE_SENTINEL"));
    assert(!strstr(result.text, "SCRIPT_POISON"));
    assert(!strstr(result.text, "STYLE_POISON"));
    assert(!strstr(result.text, "TEMPLATE_POISON"));
    assert(!strstr(result.text, "SVG_POISON"));
    assert(!strstr(result.text, "HEAD_POISON"));
    assert(!strstr(result.text, "IFRAME_POISON"));
    assert(!strstr(result.text, "OBJECT_POISON"));
    assert(!strstr(result.text, "<h1>"));
    assert(result.text_bytes == strlen(result.text));
    assert(result.text_chars > 0);
    samosa_html_result_free(&result);

    {
        static const unsigned char quoted_gt[] =
            "<html><body><p data-value=\"x > y\">kept 1 < 2 and 3 > 2</p></body></html>";
        assert(samosa_html_extract(quoted_gt, sizeof(quoted_gt) - 1,
                                   1024, &result, &error));
        assert(!strcmp(result.text, "kept 1 < 2 and 3 > 2"));
        samosa_html_result_free(&result);
    }
    {
        static const unsigned char invalid[] = "<html>bad\xc0\xaf</html>";
        assert(!samosa_html_text_valid(invalid, sizeof(invalid) - 1, &error));
        assert(!strcmp(error, "html_invalid_utf8"));
        assert(samosa_html_extract_replacing_invalid(
            invalid, sizeof(invalid) - 1, 1024, &result, &error));
        assert(strstr(result.text, "bad\xef\xbf\xbd\xef\xbf\xbd"));
        samosa_html_result_free(&result);
    }
    {
        static const unsigned char binary[] = "<html>bad\0text</html>";
        assert(!samosa_html_text_valid(binary, sizeof(binary) - 1, &error));
        assert(!strcmp(error, "html_binary_control"));
    }
    {
        static const unsigned char large[] =
            "<html><body>This readable text exceeds a tiny cap.</body></html>";
        assert(!samosa_html_extract(large, sizeof(large) - 1, 8,
                                    &result, &error));
        assert(!strcmp(error, "html_output_limit"));
    }
    {
        static const unsigned char plain[] = "ordinary <b>fragment</b>";
        assert(!samosa_html_sniff(plain, sizeof(plain) - 1));
    }

    puts("test_samosa_html: PASS");
    return 0;
}
