#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Serialization.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <cstring>

#include "Epub/BookMetadataCache.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char MEDIA_TYPE_IMAGE_PREFIX[] = "image/";
constexpr char itemCacheFile[] = "/.items.bin";

bool startsWithImageMediaType(const std::string& mediaType) {
  constexpr size_t prefixLen = sizeof(MEDIA_TYPE_IMAGE_PREFIX) - 1;
  if (mediaType.size() < prefixLen) {
    return false;
  }

  for (size_t i = 0; i < prefixLen; ++i) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(mediaType[i])));
    if (c != MEDIA_TYPE_IMAGE_PREFIX[i]) {
      return false;
    }
  }

  return true;
}

bool isXmlWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Metadata text comes straight from the (untrusted) OPF; unbounded growth on
// a multi-megabyte title would exhaust the heap. Downstream consumers truncate
// far below this anyway, so overflow is clamped, not fatal.
constexpr size_t MAX_METADATA_TEXT = 512;

void appendMetadataText(std::string& out, const XML_Char* text, const int len, bool& spacePending,
                        bool* separatorPending = nullptr) {
  if (out.size() >= MAX_METADATA_TEXT) return;  // already clamped and logged
  for (int i = 0; i < len; i++) {
    const char c = text[i];
    if (isXmlWhitespace(c)) {
      spacePending = true;
      continue;
    }

    if (out.size() >= MAX_METADATA_TEXT) {
      LOG_DBG("COF", "Metadata text exceeds %u bytes; truncating", static_cast<unsigned>(MAX_METADATA_TEXT));
      return;
    }
    if (separatorPending != nullptr && *separatorPending) {
      out.append(", ");
      *separatorPending = false;
      spacePending = false;
    } else if (spacePending && !out.empty()) {
      out.push_back(' ');
    }
    spacePending = false;
    out.push_back(c);
  }
}

void assignMetadataAttribute(std::string& out, const char* text) {
  out.clear();
  if (!text) return;
  bool spacePending = false;
  appendMetadataText(out, text, static_cast<int>(strlen(text)), spacePending);
}

bool asciiEqualsIgnoreCase(const std::string& value, const char* expected) {
  const size_t expectedLen = strlen(expected);
  if (value.size() != expectedLen) return false;
  for (size_t i = 0; i < expectedLen; i++) {
    if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(expected[i])))
      return false;
  }
  return true;
}
}  // namespace

bool ContentOpfParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("COF", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

ContentOpfParser::~ContentOpfParser() {
  destroyXmlParser(parser);
  if (metadataOnly) {
    return;
  }
  if (tempItemStore) {
    tempItemStore.close();
  }
  const auto itemCachePath = cachePath + itemCacheFile;
  if (Storage.exists(itemCachePath.c_str())) {
    Storage.remove(itemCachePath.c_str());
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);

    if (!buf) {
      LOG_ERR("COF", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("COF", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;

    if (metadataOnly && metadataComplete) {
      const size_t processed = size - remainingInBuffer;
      return processed < size ? processed : size - 1;
    }
  }

  return size;
}

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)atts;

  if (self->metadataOnly && self->metadataComplete) {
    return;
  }
  if (self->metadataOnly && (xmlLocalNameEquals(name, "manifest") || xmlLocalNameEquals(name, "spine") ||
                             xmlLocalNameEquals(name, "guide"))) {
    self->metadataComplete = true;
    return;
  }

  if (self->state == START && xmlLocalNameEquals(name, "package")) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "metadata")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "title")) {
    // Only capture the first title element; subsequent ones are subtitles
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
      self->metadataSpacePending = false;
    }
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "creator")) {
    self->state = IN_BOOK_AUTHOR;
    self->metadataSpacePending = false;
    self->authorSeparatorPending = !self->author.empty();
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "language")) {
    self->state = IN_BOOK_LANGUAGE;
    self->metadataSpacePending = false;
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "manifest")) {
    self->state = IN_MANIFEST;
    if (!Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for writing. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "spine")) {
    self->state = IN_SPINE;
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }

    // Keep the binary-search index for normal books. Generated books with thousands of
    // one-page documents use the disk-backed ordered scan below instead: an unbounded deque
    // exhausted the X3 heap and aborted inside operator new before the cover worker returned.
    if (!self->itemIndexOverflowed && !self->itemIndex.empty()) {
      std::sort(self->itemIndex.begin(), self->itemIndex.end(), [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
        return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
      });
      self->useItemIndex = true;
      LOG_DBG("COF", "Using fast index for %zu manifest items", self->itemIndex.size());
    } else if (self->itemIndexOverflowed) {
      self->itemIndex.clear();
      self->tempItemStore.seek(0);
      LOG_INF("COF", "Manifest exceeds %u items; using bounded streaming lookup",
              static_cast<unsigned>(MAX_ITEM_INDEX_ENTRIES));
    }
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "guide")) {
    self->state = IN_GUIDE;
    // TODO Remove print
    LOG_DBG("COF", "Entering guide state.");
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "meta")) {
    bool isCover = false;
    const char* content = nullptr;
    const char* metadataName = nullptr;
    const char* property = nullptr;
    const char* refines = nullptr;
    const char* id = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0 && strcmp(atts[i + 1], "cover") == 0) {
        isCover = true;
        metadataName = atts[i + 1];
      } else if (strcmp(atts[i], "name") == 0) {
        metadataName = atts[i + 1];
      } else if (strcmp(atts[i], "content") == 0) {
        content = atts[i + 1];
      } else if (strcmp(atts[i], "property") == 0) {
        property = atts[i + 1];
      } else if (strcmp(atts[i], "refines") == 0) {
        refines = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        id = atts[i + 1];
      }
    }

    if (isCover && content) {
      self->coverItemId = content;
    }

    // EPUB 2 / Calibre convention. Attribute values are bounded and whitespace
    // normalised just like dc:title, so a malformed package cannot grow these
    // long-lived strings without limit.
    if (metadataName && content && strcmp(metadataName, "calibre:series") == 0) {
      assignMetadataAttribute(self->series, content);
    } else if (metadataName && content && strcmp(metadataName, "calibre:series_index") == 0) {
      assignMetadataAttribute(self->seriesIndex, content);
    }

    // EPUB 3 collection metadata stores the values as element text and links
    // type/position refinements back to the collection id.
    self->metaTextKind = MetaTextKind::None;
    self->metaText.clear();
    const bool refinesCollection =
        refines && !self->collectionId.empty() && refines[0] == '#' && self->collectionId == refines + 1;
    if (property && strcmp(property, "belongs-to-collection") == 0) {
      self->collectionId = id ? id : "";
      self->collectionName.clear();
      self->collectionPosition.clear();
      self->collectionTypeSeen = false;
      self->collectionIsSeries = false;
      self->metaTextKind = MetaTextKind::CollectionName;
    } else if (property && refinesCollection && strcmp(property, "collection-type") == 0) {
      self->metaTextKind = MetaTextKind::CollectionType;
    } else if (property && refinesCollection && strcmp(property, "group-position") == 0) {
      self->metaTextKind = MetaTextKind::CollectionPosition;
    }
    self->metadataSpacePending = false;
    return;
  }

  if (self->state == IN_MANIFEST && xmlLocalNameEquals(name, "item")) {
    std::string itemId;
    std::string href;
    std::string mediaType;
    std::string properties;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        itemId = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      } else if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "properties") == 0) {
        properties = atts[i + 1];
      }
    }

    // Record an index entry for fast lookup later, but never let untrusted package size
    // dictate long-lived heap growth. Once full, the spine pass streams .items.bin in order.
    if (self->tempItemStore && !self->itemIndexOverflowed) {
      if (self->itemIndex.size() >= MAX_ITEM_INDEX_ENTRIES) {
        self->itemIndexOverflowed = true;
      } else {
        ItemIndexEntry entry;
        entry.idHash = fnvHash(itemId);
        entry.idLen = static_cast<uint16_t>(itemId.size());
        entry.fileOffset = static_cast<uint32_t>(self->tempItemStore.position());
        self->itemIndex.push_back(entry);
      }
    }

    // Write items down to SD card
    serialization::writeString(self->tempItemStore, itemId);
    serialization::writeString(self->tempItemStore, href);

    if (itemId == self->coverItemId) {
      // Some EPUBs set meta name="cover" to an XHTML wrapper item.
      // Only treat it as a cover image when the manifest media-type is image/*.
      if (startsWithImageMediaType(mediaType)) {
        self->coverItemHref = href;
      } else {
        LOG_DBG("COF", "Ignoring meta cover item '%s' with non-image media type: %s", itemId.c_str(),
                mediaType.c_str());
      }
    }

    if (mediaType == MEDIA_TYPE_NCX) {
      if (self->tocNcxPath.empty()) {
        self->tocNcxPath = href;
      } else {
        LOG_DBG("COF", "Warning: Multiple NCX files found in manifest. Ignoring duplicate: %s", href.c_str());
      }
    }

    // Collect CSS files
    if (mediaType == MEDIA_TYPE_CSS) {
      self->cssFiles.push_back(href);
    }

    // EPUB 3: Check for nav document (properties contains "nav")
    if (!properties.empty() && self->tocNavPath.empty()) {
      // Properties is space-separated, check if "nav" is present as a word
      if (properties == "nav" || properties.find("nav ") == 0 || properties.find(" nav") != std::string::npos) {
        self->tocNavPath = href;
        LOG_DBG("COF", "Found EPUB 3 nav document: %s", href.c_str());
      }
    }

    // EPUB 3: Check for cover image (properties contains "cover-image")
    if (!properties.empty() && self->coverItemHref.empty()) {
      if (properties == "cover-image" || properties.find("cover-image ") == 0 ||
          properties.find(" cover-image") != std::string::npos) {
        self->coverItemHref = href;
      }
    }
    return;
  }

  // NOTE: This relies on spine appearing after item manifest (which is pretty safe as it's part of the EPUB spec)
  // Only run the spine parsing if there's a cache to add it to
  if (self->cache) {
    if (self->state == IN_SPINE && xmlLocalNameEquals(name, "itemref")) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "idref") == 0) {
          const std::string idref = atts[i + 1];
          std::string href;
          bool found = false;

          if (self->useItemIndex) {
            // Fast path: binary search
            uint32_t targetHash = fnvHash(idref);
            uint16_t targetLen = static_cast<uint16_t>(idref.size());

            auto it = std::lower_bound(self->itemIndex.begin(), self->itemIndex.end(),
                                       ItemIndexEntry{targetHash, targetLen, 0},
                                       [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
                                         return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
                                       });

            // Check for match (may need to check a few due to hash collisions)
            while (it != self->itemIndex.end() && it->idHash == targetHash) {
              self->tempItemStore.seek(it->fileOffset);
              std::string itemId;
              serialization::readString(self->tempItemStore, itemId);
              if (itemId == idref) {
                serialization::readString(self->tempItemStore, href);
                found = true;
                break;
              }
              ++it;
            }
          } else {
            found = self->findItemHrefSequential(idref, href);
          }

          if (found && self->cache) {
            self->cache->createSpineEntry(href);
          }
        }
      }
      return;
    }
  }
  // parse the guide
  if (self->state == IN_GUIDE && xmlLocalNameEquals(name, "reference")) {
    std::string type;
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        guideHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      }
    }
    if (!guideHref.empty()) {
      // EPUB 2 guides often mark every content file as "text", so that type
      // does not identify a reliable first-reading location. Only use the
      // explicit "start" semantic; otherwise the reader opens at spine index 0.
      if (type == "start" && !self->hasExplicitStartReference) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type.c_str(), guideHref.c_str());
        self->textReferenceHref = guideHref;
        self->hasExplicitStartReference = type == "start";
      } else if ((type == "cover" || type == "cover-page") && self->guideCoverPageHref.empty()) {
        LOG_DBG("COF", "Found cover reference in guide: %s", guideHref.c_str());
        self->guideCoverPageHref = guideHref;
      }
    }
    return;
  }
}

bool ContentOpfParser::findItemHrefSequential(const std::string& idref, std::string& href) {
  // EPUB generators almost always emit manifest and spine in the same order. Continue from
  // the previous match, making a 4,000-item book O(n), and wrap once for legal out-of-order
  // spines. The old fallback restarted at zero for every itemref (O(n^2)).
  const uint32_t startOffset = static_cast<uint32_t>(tempItemStore.position());
  std::string itemId;
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) {
      if (startOffset == 0) break;
      tempItemStore.seek(0);
    }
    while (tempItemStore.available() && (pass == 0 || static_cast<uint32_t>(tempItemStore.position()) < startOffset)) {
      if (!serialization::readString(tempItemStore, itemId) || !serialization::readString(tempItemStore, href)) {
        href.clear();
        tempItemStore.seek(startOffset);
        return false;
      }
      if (itemId == idref) return true;
    }
  }
  href.clear();
  tempItemStore.seek(startOffset);
  return false;
}

void XMLCALL ContentOpfParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);

  if (self->metadataOnly && self->metadataComplete) {
    return;
  }

  if (self->state == IN_BOOK_TITLE) {
    appendMetadataText(self->title, s, len, self->metadataSpacePending);
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    appendMetadataText(self->author, s, len, self->metadataSpacePending, &self->authorSeparatorPending);
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE) {
    appendMetadataText(self->language, s, len, self->metadataSpacePending);
    return;
  }

  if (self->state == IN_METADATA && self->metaTextKind != MetaTextKind::None) {
    appendMetadataText(self->metaText, s, len, self->metadataSpacePending);
    return;
  }
}

void XMLCALL ContentOpfParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)name;

  if (self->metadataOnly && self->metadataComplete) {
    return;
  }

  if (self->state == IN_SPINE && xmlLocalNameEquals(name, "spine")) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_GUIDE && xmlLocalNameEquals(name, "guide")) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_MANIFEST && xmlLocalNameEquals(name, "manifest")) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_BOOK_TITLE && xmlLocalNameEquals(name, "title")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && xmlLocalNameEquals(name, "creator")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && xmlLocalNameEquals(name, "language")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && self->metaTextKind != MetaTextKind::None && xmlLocalNameEquals(name, "meta")) {
    switch (self->metaTextKind) {
      case MetaTextKind::CollectionName:
        self->collectionName = std::move(self->metaText);
        break;
      case MetaTextKind::CollectionType:
        self->collectionTypeSeen = true;
        self->collectionIsSeries = asciiEqualsIgnoreCase(self->metaText, "series");
        break;
      case MetaTextKind::CollectionPosition:
        self->collectionPosition = std::move(self->metaText);
        break;
      case MetaTextKind::None:
        break;
    }
    self->metaText.clear();
    self->metaTextKind = MetaTextKind::None;
    return;
  }

  if (self->state == IN_METADATA && xmlLocalNameEquals(name, "metadata")) {
    // Calibre metadata wins when both forms exist. An untyped EPUB 3
    // collection is accepted because many exporters omit collection-type; an
    // explicitly non-series collection is not presented as a book series.
    if (self->series.empty() && !self->collectionName.empty() &&
        (!self->collectionTypeSeen || self->collectionIsSeries)) {
      self->series = std::move(self->collectionName);
      self->seriesIndex = std::move(self->collectionPosition);
    }
    self->state = IN_PACKAGE;
    self->metadataComplete = true;
    return;
  }

  if (self->state == IN_PACKAGE && xmlLocalNameEquals(name, "package")) {
    self->state = START;
    return;
  }
}
