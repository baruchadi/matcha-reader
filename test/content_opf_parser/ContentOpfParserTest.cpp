#include <gtest/gtest.h>

#include <string>

#include "ContentOpfParser.h"

namespace {

void parse(ContentOpfParser& parser, const std::string& xml) {
  ASSERT_TRUE(parser.setup());
  EXPECT_EQ(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
}

}  // namespace

TEST(ContentOpfParserMetadata, EntityCallbackDoesNotSplitOneAuthor) {
  const std::string xml =
      R"(<package xmlns:dc="urn:dc"><metadata><dc:creator>&#201;mile Zola</dc:creator></metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.author, "Émile Zola");
}

TEST(ContentOpfParserMetadata, ClampsOversizedMetadataTextInsteadOfGrowingUnbounded) {
  const std::string hugeTitle(64 * 1024, 'A');
  const std::string xml =
      "<package xmlns:dc=\"urn:dc\"><metadata><dc:title>" + hugeTitle + " tail</dc:title></metadata></package>";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title.size(), 512u);
  EXPECT_EQ(parser.title[0], 'A');
}

TEST(ContentOpfParserMetadata, SeparatesCreatorElementsAndCollapsesXmlWhitespace) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>  The
   Left Hand   of Darkness  </dc:title>
    <dc:creator> Ursula   K. Le Guin </dc:creator>
    <dc:creator>
Octavia E. Butler
</dc:creator>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title, "The Left Hand of Darkness");
  EXPECT_EQ(parser.author, "Ursula K. Le Guin, Octavia E. Butler");
}

TEST(ContentOpfParserMetadata, StopsBeforeManifestWithoutOpeningTemporaryStorage) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>A Wizard of Earthsea</dc:title>
    <dc:creator>Ursula K. Le Guin</dc:creator>
    <dc:language>en</dc:language>
  </metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
  </package>)";
  Storage = {};
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr, true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(parser.title, "A Wizard of Earthsea");
  EXPECT_EQ(parser.author, "Ursula K. Le Guin");
  EXPECT_EQ(parser.language, "en");
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserMetadata, NeverEntersManifestWhenMetadataElementIsMissing) {
  const std::string xml =
      R"(<package><manifest><item id="chapter" href="chapter.xhtml"/></manifest><spine/></package>)";
  Storage = {};
  ContentOpfParser parser("/missing-cache", "OPS/", xml.size(), nullptr, true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserMetadata, ReadsCalibreSeriesAndPosition) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>The Wise Man's Fear</dc:title>
    <meta name="calibre:series" content="The Kingkiller Chronicle"/>
    <meta name="calibre:series_index" content="2.0"/>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "The Kingkiller Chronicle");
  EXPECT_EQ(parser.seriesIndex, "2.0");
}

TEST(ContentOpfParserMetadata, ReadsEpub3SeriesRefinements) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <dc:title>Toradora 2</dc:title>
    <meta property="belongs-to-collection" id="toradora">Toradora!</meta>
    <meta refines="#toradora" property="collection-type">series</meta>
    <meta refines="#toradora" property="group-position">2</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.series, "Toradora!");
  EXPECT_EQ(parser.seriesIndex, "2");
}

TEST(ContentOpfParserMetadata, DoesNotTreatExplicitNonSeriesCollectionAsSeries) {
  const std::string xml = R"(<package xmlns:dc="urn:dc"><metadata>
    <meta property="belongs-to-collection" id="publisher-set">Classroom Editions</meta>
    <meta refines="#publisher-set" property="collection-type">set</meta>
    <meta refines="#publisher-set" property="group-position">4</meta>
  </metadata></package>)";
  ContentOpfParser parser("", "", xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_TRUE(parser.series.empty());
  EXPECT_TRUE(parser.seriesIndex.empty());
}
