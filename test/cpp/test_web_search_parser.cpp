// Offline fixture test for the keyless web-search RSS parser.
#include "lemon/web_search.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#define CHECK(condition) do { \
    if (!(condition)) throw std::runtime_error("Web search parser check failed: " #condition); \
} while (false)

int main() {
    using lemon::web_search::parse_rss;
    const std::string fixture = R"(<rss version="2.0"><channel>
      <item><title>A &amp; B</title><link>https://example.org/a?x=1&amp;y=2</link>
        <description>Clean &lt;b&gt;snippet&lt;/b&gt; &amp; &#169;</description></item>
      <item><title>A duplicate</title><link>https://example.org/a?x=1&amp;y=2</link></item>
      <item><title>Skip HTTP</title><link>http://example.org/untrusted</link></item>
      <item><title>Second</title><link>https://example.net/b</link><description></description></item>
      <item><title>Third</title><link>https://example.com/c</link></item>
    </channel></rss>)";
    const auto parsed = parse_rss(fixture, 2);
    CHECK(parsed["results"].size() == 2);
    CHECK(parsed["truncated"] == true);
    CHECK(parsed["results"][0]["title"] == "A & B");
    CHECK(parsed["results"][0]["url"] == "https://example.org/a?x=1&y=2");
    CHECK(parsed["results"][0]["snippet"] == "Clean snippet & ©");
    CHECK(parsed["results"][1]["snippet"] == "");
    CHECK(parsed["results"][1]["url"] == "https://example.net/b");
    CHECK(parse_rss("<rss><channel/></rss>", 5)["results"].empty());
    bool threw = false;
    try { (void)parse_rss("<html>Captcha</html>", 5); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    const auto html = parse_rss(
        "<rss><item><title>Title</title><link>https://example.org/</link>"
        "<description>&lt;script&gt;ignore me&lt;/script&gt;Safe</description></item></rss>", 5);
    CHECK(html["results"][0]["snippet"] == "Safe");
    const auto closing_prefix = parse_rss(
        "<rss><item><title>Title</title><link>https://example.org/script</link>"
        "<description>&lt;script&gt;hide &lt;/script-example&gt; more hidden"
        "&lt;/ScRiPt &gt;Visible</description></item></rss>", 5);
    CHECK(closing_prefix["results"].size() == 1);
    CHECK(closing_prefix["results"][0]["snippet"] == "Visible");
    const auto less_than_title = parse_rss(
        "<rss><item><title>Why x &lt; y matters</title>"
        "<link>https://example.org/less-than</link></item></rss>", 5);
    CHECK(less_than_title["results"].size() == 1);
    CHECK(less_than_title["results"][0]["title"] == "Why x < y matters");
    const auto cdata = parse_rss(
        "<rss><item><title><![CDATA[How to write &amp; in XML]]></title>"
        "<link><![CDATA[https://example.org/a?x=1&amp;y=2]]></link>"
        "<description><![CDATA[Keep &amp; literal]]></description></item></rss>", 5);
    CHECK(cdata["results"].size() == 1);
    CHECK(cdata["results"][0]["title"] == "How to write &amp; in XML");
    CHECK(cdata["results"][0]["url"] == "https://example.org/a?x=1&amp;y=2");
    CHECK(cdata["results"][0]["snippet"] == "Keep &amp; literal");
    const auto encoded_html = parse_rss(
        "<rss><item><title>HTML</title><link>https://example.org/html</link>"
        "<description>&lt;p&gt;Research &amp;amp; development&lt;/p&gt;</description>"
        "</item></rss>", 5);
    CHECK(encoded_html["results"][0]["snippet"] == "Research & development");
    const auto cdata_html = parse_rss(
        "<rss><item><title>CDATA HTML</title><link>https://example.org/cdata-html</link>"
        "<description><![CDATA[<p>Research &amp; development</p>]]></description>"
        "</item></rss>", 5);
    CHECK(cdata_html["results"][0]["snippet"] == "Research & development");
    const auto bing = parse_rss(
        "<rss><item><title>Title</title><link>https://www.bing.com/news/apiclick.aspx?"
        "ref=rss&amp;url=https%3A%2F%2Fexample.com%2Fstory%3Fx%3D1</link>"
        "<description>Snippet</description></item></rss>", 5);
    CHECK(bing["results"].size() == 1);
    CHECK(bing["results"][0]["url"] == "https://example.com/story?x=1");
    const auto legacy_bing = parse_rss(
        "<rss><item><title>Title</title><link>http://www.bing.com/news/apiclick.aspx?"
        "ref=rss&amp;url=https%3A%2F%2Fexample.net%2Farticle</link></item></rss>", 5);
    CHECK(legacy_bing["results"].size() == 1);
    CHECK(legacy_bing["results"][0]["url"] == "https://example.net/article");
    const auto forbidden = parse_rss(
        "<rss><item><title>Tracking only</title><link>https://www.bing.com/ck/a?u=xyz"
        "</link></item><item><title>Credential</title><link>https://user@host.example/a"
        "</link></item><item><title>Scripted</title><link>https://example.com</link>"
        "<description>&lt;style&gt;secret&lt;/style&gt;visible</description></item></rss>", 5);
    CHECK(forbidden["results"].size() == 1);
    CHECK(forbidden["results"][0]["snippet"] == "visible");
    CHECK(lemon::web_search::canonical_link("https://example.org/@reporter/story") ==
          "https://example.org/@reporter/story");
    CHECK(lemon::web_search::canonical_link("https://example.org/?user=@reporter") ==
          "https://example.org/?user=@reporter");
    CHECK(lemon::web_search::canonical_link("https://user@example.org/story").empty());
    CHECK(lemon::web_search::canonical_link("https://example.org/report#findings") ==
          "https://example.org/report#findings");
    CHECK(lemon::web_search::canonical_link("https://example.org#section@reader") ==
          "https://example.org#section@reader");
    CHECK(lemon::web_search::canonical_link("https://user@example.org#fragment").empty());
    CHECK(lemon::web_search::canonical_link("https://example.org/" + std::string(1, '\xFF')).empty());
    CHECK(lemon::web_search::canonical_link(
          "https://www.bing.com/news/apiclick.aspx?url=https%3A%2F%2Fexample.org%2F%FF").empty());
    CHECK(lemon::web_search::canonical_link("https://example.org/a%FF")==
          "https://example.org/a%FF");
    // Leading whitespace must not bypass tracking-link rejection or wrapper decoding.
    CHECK(lemon::web_search::canonical_link(" \n https://www.bing.com/ck/a?u=xyz \t").empty());
    CHECK(lemon::web_search::canonical_link(
        "https://www.bing.com/news/apiclick.aspx?url=%20https%3A%2F%2Fwww.bing.com%2Fck%2Fa%3Fu%3Dx%20"
    ).empty());
    CHECK(lemon::web_search::canonical_link(
        " \t http://www.bing.com/news/apiclick.aspx?ref=rss&url=%20https%3A%2F%2Fexample.net%2Fpadded%20 \n") ==
        "https://example.net/padded");
    const auto padded_bing = parse_rss(
        "<rss><item><title>Padded wrapper</title><link> \n https://www.bing.com/news/apiclick.aspx?"
        "ref=rss&amp;url=%20https%3A%2F%2Fexample.org%2Fstory%20 \t</link></item>"
        "<item><title>Padded tracking</title><link> \t https://www.bing.com/ck/a?u=x "
        "</link></item></rss>", 5);
    CHECK(padded_bing["results"].size() == 1);
    CHECK(padded_bing["results"][0]["url"] == "https://example.org/story");

    // Truncating at 180 bytes must never split a 2-, 3-, or 4-byte UTF-8 codepoint.
    for (const std::string& character : {std::string("\xC3\xA9"),
                                         std::string("\xE2\x82\xAC"),
                                         std::string("\xF0\x9F\x9A\x80")}) {
        for (size_t prefix = 175; prefix <= 181; ++prefix) {
            const std::string title = std::string(prefix, 'a') + character;
            const auto utf8 = parse_rss("<rss><item><title>" + title +
                "</title><link>https://example.org/utf8</link></item></rss>", 5);
            const std::string expected = std::string(std::min(prefix, size_t{180}), 'a') +
                (prefix + character.size() <= 180 ? character : "");
            CHECK(utf8["results"][0]["title"] == expected);
            CHECK(!utf8.dump().empty());  // nlohmann/json rejects malformed UTF-8 here.
        }
    }
    const auto snippet_boundary = parse_rss(
        "<rss><item><title>Boundary</title><link>https://example.org/boundary</link>"
        "<description>" + std::string(419, 'x') + "\xC3\xA9" +
        "</description></item></rss>", 5);
    CHECK(snippet_boundary["results"][0]["snippet"] == std::string(419, 'x'));
    CHECK(!snippet_boundary.dump().empty());
    const auto invalid_utf8 = parse_rss(
        "<rss><item><title>A" + std::string("\xC0\xAF") +
        "B</title><link>https://example.org/invalid</link></item></rss>", 5);
    CHECK(invalid_utf8["results"][0]["title"] == "AB");
    CHECK(!invalid_utf8.dump().empty());
    CHECK(lemon::web_search::clean_text("<ScRiPt>hidden</sCrIpT><StYlE>css</stYle>Visible", 180)
          == "Visible");
    std::string many_tags;
    for (size_t i = 0; i < 1500; ++i) many_tags += "<b></b>";
    CHECK(lemon::web_search::clean_text(many_tags + "Visible", 180) == "Visible");

    const auto news = parse_rss(
        "<rss xmlns:News=\"urn:news\"><channel><item>"
        "<title>Actual headline</title><link>https://example.org/news</link>"
        "<pubDate>Wed, 18 Nov 2026 11:15:00 GMT</pubDate>"
        "<News:Source>Example Daily</News:Source>"
        "<description>Published story</description></item></channel></rss>", 5);
    CHECK(news["results"].size() == 1);
    CHECK(news["results"][0]["published_at"] == "Wed, 18 Nov 2026 11:15:00 GMT");
    CHECK(news["results"][0]["source"] == "Example Daily");
    CHECK(news["results"][0]["url"] == "https://example.org/news");
    CHECK(!parsed["results"][0].contains("published_at"));
    CHECK(!parsed["results"][0].contains("source"));
    // max_results is validated before converting to int (no integer wraparound).
    for (const auto& value : {lemon::web_search::json(0), lemon::web_search::json(6),
                              lemon::web_search::json(std::int64_t{4294967297LL})}) {
        bool invalid = false;
        try { (void)lemon::web_search::search({{"query", "test"}, {"max_results", value}}); }
        catch (const std::invalid_argument&) { invalid = true; }
        CHECK(invalid);
    }
    CHECK(lemon::web_search::search_url("MCP SDK 2026", "web") ==
          "https://www.bing.com/search?format=rss&q=MCP%20SDK%202026");
    CHECK(lemon::web_search::search_url("WELT News", "news") ==
          "https://www.bing.com/news/search?format=rss&q=WELT%20News");
}
