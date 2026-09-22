#pragma once
#include <Print.h>

#include <algorithm>
#include <deque>
#include <vector>

#include "Epub.h"
#include "expat.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };
  enum class MetaTextKind : uint8_t { None, CollectionName, CollectionType, CollectionPosition };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  const bool metadataOnly;
  bool metadataComplete = false;
  HalFile tempItemStore;
  std::string coverItemId;
  bool hasExplicitStartReference = false;
  // XML character data is allowed to arrive in several callbacks for one text
  // node (notably around character references). Keep whitespace and creator
  // separation as element state rather than inferring either from callbacks.
  bool metadataSpacePending = false;
  bool authorSeparatorPending = false;
  MetaTextKind metaTextKind = MetaTextKind::None;
  std::string metaText;
  std::string collectionId;
  std::string collectionName;
  std::string collectionPosition;
  bool collectionTypeSeen = false;
  bool collectionIsSeries = false;

  // Index for fast idref→href lookup (binary search over .items.bin)
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  std::deque<ItemIndexEntry> itemIndex;
  bool useItemIndex = false;
  bool itemIndexOverflowed = false;
  // A normal book stays well below this (the next-largest book on the reported card has
  // 415 items). Large generated books can have thousands; retaining every entry consumed
  // 50KB+ and made std::deque abort under -fno-exceptions on the X3.
  static constexpr size_t MAX_ITEM_INDEX_ENTRIES = 512;

  // FNV-1a hash function
  static uint32_t fnvHash(const std::string& s) {
    uint32_t hash = 2166136261u;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 16777619u;
    }
    return hash;
  }

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);
  bool findItemHrefSequential(const std::string& idref, std::string& href);

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string series;
  std::string seriesIndex;
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache, const bool metadataOnly = false)
      : cachePath(cachePath),
        baseContentPath(baseContentPath),
        remainingSize(xmlSize),
        cache(cache),
        metadataOnly(metadataOnly) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
