#pragma once

#include <utility>
#include <vector>

#include "dxvk_buffer.h"
#include "dxvk_cmdlist.h"
#include "dxvk_image.h"

namespace dxvk {

  /**
   * \brief Address range
   */
  struct DxvkAddressRange {
    /// Unique resource handle or address
    uint64_t resource = 0u;
    /// Range start. For buffers, this shall be a byte offset,
    /// images can encode the first subresource index here.
    uint32_t rangeStart = 0u;
    /// Range end. For buffers, this is the offset of the last byte
    /// included in the range, i.e. offset + size - 1. For images,
    /// this is the last subresource included in the range.
    uint32_t rangeEnd = 0u;

    bool contains(const DxvkAddressRange& other) const {
      return resource == other.resource
          && rangeStart <= other.rangeStart
          && rangeEnd >= other.rangeEnd;
    }

    bool overlaps(const DxvkAddressRange& other) const {
      return resource == other.resource
          && rangeEnd >= other.rangeStart
          && rangeStart <= other.rangeEnd;
    }

    bool lt(const DxvkAddressRange& other) const {
      return (resource < other.resource)
          || (resource == other.resource && rangeStart < other.rangeStart);
    }
  };


  /**
   * \brief Barrier tree node
   *
   * Node of a red-black tree, consisting of a packed node
   * header as well as aresource address range. GCC generates
   * weird code with bitfields here, so pack manually.
   */
  struct DxvkBarrierTreeNode {
    constexpr static uint64_t NodeIndexMask = (1u << 21) - 1u;

    // Packed header with node indices and the node color.
    // [0:0]: Set if the node is red, clear otherwise.
    // [21:1]: Index of the left child node, may be 0.
    // [42:22]: Index of the right child node, may be 0.
    // [43:63]: Index of the parent node, may be 0 for the root.
    uint64_t header = 0u;

    // Address range of the node
    DxvkAddressRange addressRange = { };

    void setRed(bool red) {
      header &= ~uint64_t(1u);
      header |= uint64_t(red);
    }

    bool isRed() const {
      return header & 1u;
    }

    void setParent(uint32_t node) {
      header &= ~(NodeIndexMask << 43);
      header |= uint64_t(node) << 43;
    }

    void setChild(uint32_t index, uint32_t node) {
      uint32_t shift = (index ? 22 : 1);
      header &= ~(NodeIndexMask << shift);
      header |= uint64_t(node) << shift;
    }

    uint32_t parent() const {
      return uint32_t((header >> 43) & NodeIndexMask);
    }

    uint32_t child(uint32_t index) const {
      uint32_t shift = (index ? 22 : 1);
      return uint32_t((header >> shift) & NodeIndexMask);
    }

    bool isRoot() const {
      return parent() == 0u;
    }
  };


  /**
   * \brief Barrier tracker
   *
   * Provides a two-part hash table for read and written resource
   * ranges, which is backed by binary trees to handle individual
   * address ranges as well as collisions.
   */
  class DxvkBarrierTracker {
    constexpr static uint32_t HashTableSize = 32u;
  public:

    DxvkBarrierTracker();

    ~DxvkBarrierTracker();

    /**
     * \brief Checks whether there is a pending access of a given type
     *
     * \param [in] range Resource range
     * \param [in] accessType Access type
     * \returns \c true if the range has a pending access
     */
    bool findRange(
      const DxvkAddressRange&           range,
            DxvkAccess                  accessType) const;

    /**
     * \brief Inserts address range for a given access type
     *
     * \param [in] range Resource range
     * \param [in] accessType Access type
     */
    void insertRange(
      const DxvkAddressRange&           range,
            DxvkAccess                  accessType);

    /**
     * \brief Clears the entire structure
     *
     * Invalidates all hash table entries and trees.
     */
    void clear();

    /**
     * \brief Checks whether any resources are dirty
     * \returns \c true if the tracker is empty.
     */
    bool empty() const {
      return !m_rootMaskValid;
    }

  private:

    uint64_t m_rootMaskValid = 0u;
    uint64_t m_rootMaskSubtree = 0u;

    std::vector<DxvkBarrierTreeNode>  m_nodes;
    std::vector<uint32_t>             m_free;

    uint32_t allocateNode();

    void freeNode(uint32_t node);

    uint32_t findNode(
      const DxvkAddressRange&           range,
            uint32_t                    rootIndex) const;

    uint32_t insertNode(
      const DxvkAddressRange&           range,
            uint32_t                    rootIndex);

    void removeNode(
            uint32_t                    nodeIndex,
            uint32_t                    rootIndex);

    void rebalancePostInsert(
            uint32_t                    nodeIndex,
            uint32_t                    rootIndex);

    void rotateLeft(
            uint32_t                    nodeIndex,
            uint32_t                    rootIndex);

    void rotateRight(
            uint32_t                    nodeIndex,
            uint32_t                    rootIndex);

    static uint32_t computeRootIndex(
      const DxvkAddressRange&           range,
            DxvkAccess                  access) {
      // TODO revisit once we use internal allocation
      // objects or resource cookies here.
      size_t hash = size_t(range.resource) * 93887;
             hash ^= (hash >> 16);

      // Reserve the upper half of the implicit hash table for written
      // ranges, and add 1 because 0 refers to the actual null node.
      return 1u + (hash % HashTableSize) + (access == DxvkAccess::Write ? HashTableSize : 0u);
    }

  };


  /**
   * \brief Barrier batch
   *
   * Simple helper class to accumulate barriers that can then
   * be recorded into a command buffer in a single step.
   */
  class DxvkBarrierBatch {

  public:

    DxvkBarrierBatch(DxvkCmdBuffer cmdBuffer);
    ~DxvkBarrierBatch();

    /**
     * \brief Adds a memory barrier
     *
     * Host read access will only be flushed
     * at the end of a command list.
     * \param [in] barrier Memory barrier
     */
    void addMemoryBarrier(
      const VkMemoryBarrier2&           barrier);

    /**
     * \brief Adds an image barrier
     *
     * This will automatically turn into a normal memory barrier
     * if no queue family ownership transfer or layout transition
     * happens.
     * \param [in] barrier Memory barrier
     */
    void addImageBarrier(
      const VkImageMemoryBarrier2&      barrier);

    /**
     * \brief Flushes batched memory barriers
     * \param [in] list Command list
     */
    void flush(
      const Rc<DxvkCommandList>&        list);

    /**
     * \brief Flushes batched memory and host barriers
     * \param [in] list Command list
     */
    void finalize(
      const Rc<DxvkCommandList>&        list);

  private:

    DxvkCmdBuffer         m_cmdBuffer;

    VkMemoryBarrier2      m_memoryBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };

    VkPipelineStageFlags2 m_hostSrcStages = 0u;
    VkAccessFlags2        m_hostDstAccess = 0u;

    std::vector<VkImageMemoryBarrier2> m_imageBarriers = { };

  };


  /**
   * \brief Buffer slice for barrier tracking
   *
   * Stores the offset and length of a buffer slice,
   * as well as access flags for the given range.
   */
  class DxvkBarrierBufferSlice {

  public:

    DxvkBarrierBufferSlice()
    : m_loAddr(0), m_hiAddr(0), m_access(0) { }

    DxvkBarrierBufferSlice(VkDeviceSize offset, VkDeviceSize length, DxvkAccessFlags access)
    : m_loAddr(offset),
      m_hiAddr(offset + length),
      m_access(access) { }

    /**
     * \brief Checks whether two slices overlap
     *
     * \param [in] slice The other buffer slice to check
     * \returns \c true if the two slices overlap
     */
    bool overlaps(const DxvkBarrierBufferSlice& slice) const {
      return m_hiAddr > slice.m_loAddr
          && m_loAddr < slice.m_hiAddr;
    }

    /**
     * \brief Checks whether a given slice is dirty
     *
     * \param [in] slice The buffer slice to check
     * \returns \c true if the two slices overlap, and if
     *    at least one of the two slices have write access.
     */
    bool isDirty(const DxvkBarrierBufferSlice& slice) const {
      return (slice.m_access | m_access).test(DxvkAccess::Write) && overlaps(slice);
    }

    /**
     * \brief Checks whether two slices can be merged
     *
     * Two buffer slices can be merged if they overlap or are adjacent
     * and if the access flags are the same, or alternatively, if the
     * offset and size are the same and only the access flags differ.
     * \param [in] slice The other buffer slice to check
     * \returns \c true if the slices can be merged.
     */
    bool canMerge(const DxvkBarrierBufferSlice& slice) const {
      if (m_access == slice.m_access) {
        return m_hiAddr >= slice.m_loAddr
            && m_loAddr <= slice.m_hiAddr;
      } else {
        return m_loAddr == slice.m_loAddr
            && m_hiAddr == slice.m_hiAddr;
      }
    }

    /**
     * \brief Merges two buffer slices
     *
     * The resulting slice is guaranteed to fully contain both slices,
     * including their access flags. If called when \c canMerge would
     * return \c false, this will be a strict superset of both slices.
     * \param [in] slice The slice to merge
     */
    void merge(const DxvkBarrierBufferSlice& slice) {
      m_loAddr = std::min(m_loAddr, slice.m_loAddr);
      m_hiAddr = std::max(m_hiAddr, slice.m_hiAddr);
      m_access.set(slice.m_access);
    }

    /**
     * \brief Queries access flags
     * \returns Access flags
     */
    DxvkAccessFlags getAccess() const {
      return DxvkAccessFlags(m_access);
    }

  private:

    VkDeviceSize    m_loAddr;
    VkDeviceSize    m_hiAddr;
    DxvkAccessFlags m_access;

  };


  /**
   * \brief Image slice for barrier tracking
   *
   * Stores an image subresource range, as well as
   * access flags for the given image subresources.
   */
  class DxvkBarrierImageSlice {

  public:

    DxvkBarrierImageSlice()
    : m_aspects(0),
      m_minLayer(0),
      m_maxLayer(0),
      m_minLevel(0),
      m_maxLevel(0),
      m_access(0) { }

    DxvkBarrierImageSlice(VkImageSubresourceRange range, DxvkAccessFlags access)
    : m_aspects(range.aspectMask),
      m_minLayer(range.baseArrayLayer),
      m_maxLayer(range.baseArrayLayer + range.layerCount),
      m_minLevel(range.baseMipLevel),
      m_maxLevel(range.baseMipLevel + range.levelCount),
      m_access(access) { }

    /**
     * \brief Checks whether two slices overlap
     *
     * \param [in] slice The other image slice to check
     * \returns \c true if the two slices overlap
     */
    bool overlaps(const DxvkBarrierImageSlice& slice) const {
      return (m_aspects & slice.m_aspects)
          && (m_minLayer < slice.m_maxLayer)
          && (m_maxLayer > slice.m_minLayer)
          && (m_minLevel < slice.m_maxLevel)
          && (m_maxLevel > slice.m_minLevel);
    }

    /**
     * \brief Checks whether a given slice is dirty
     *
     * \param [in] slice The image slice to check
     * \returns \c true if the two slices overlap, and if
     *    at least one of the two slices have write access.
     */
    bool isDirty(const DxvkBarrierImageSlice& slice) const {
      return (slice.m_access | m_access).test(DxvkAccess::Write) && overlaps(slice);
    }

    /**
     * \brief Checks whether two slices can be merged
     *
     * This is a simplified implementation that does only
     * checks for adjacent subresources in one dimension.
     * \param [in] slice The other image slice to check
     * \returns \c true if the slices can be merged.
     */
    bool canMerge(const DxvkBarrierImageSlice& slice) const {
      bool sameLayers = m_minLayer == slice.m_minLayer
                     && m_maxLayer == slice.m_maxLayer;
      bool sameLevels = m_minLevel == slice.m_minLevel
                     && m_maxLevel == slice.m_maxLevel;

      if (sameLayers == sameLevels)
        return sameLayers;

      if (m_access != slice.m_access)
        return false;

      if (sameLayers) {
        return m_maxLevel >= slice.m_minLevel
            && m_minLevel <= slice.m_maxLevel;
      } else /* if (sameLevels) */ {
        return m_maxLayer >= slice.m_minLayer
            && m_minLayer <= slice.m_maxLayer;
      }
    }

    /**
     * \brief Merges two image slices
     *
     * The resulting slice is guaranteed to fully contain both slices,
     * including their access flags. If called when \c canMerge would
     * return \c false, this will be a strict superset of both slices.
     * \param [in] slice The slice to merge
     */
    void merge(const DxvkBarrierImageSlice& slice) {
      m_aspects |= slice.m_aspects;
      m_minLayer = std::min(m_minLayer, slice.m_minLayer);
      m_maxLayer = std::max(m_maxLayer, slice.m_maxLayer);
      m_minLevel = std::min(m_minLevel, slice.m_minLevel);
      m_maxLevel = std::max(m_maxLevel, slice.m_maxLevel);
      m_access.set(slice.m_access);
    }

    /**
     * \brief Queries access flags
     * \returns Access flags
     */
    DxvkAccessFlags getAccess() const {
      return m_access;
    }

  private:

    VkImageAspectFlags  m_aspects;
    uint32_t            m_minLayer;
    uint32_t            m_maxLayer;
    uint32_t            m_minLevel;
    uint32_t            m_maxLevel;
    DxvkAccessFlags     m_access;

  };


  /**
   * \brief Resource slice set for barrier tracking
   *
   * Implements a versioned hash table for fast resource
   * lookup, with a single-linked list accurately storing
   * each accessed slice if necessary.
   * \tparam K Resource handle type
   * \tparam T Resource slice type
   */
  template<typename K, typename T>
  class DxvkBarrierSubresourceSet {
    constexpr static uint32_t NoEntry = ~0u;
  public:

    /**
     * \brief Queries access flags of a given resource slice
     *
     * \param [in] resource Resource handle
     * \param [in] slice Resource slice
     * \returns Or'd access flags of all known slices
     *    that overlap with the given slice.
     */
    DxvkAccessFlags getAccess(K resource, const T& slice) {
      HashEntry* entry = findHashEntry(resource);

      if (!entry)
        return DxvkAccessFlags();

      // Exit early if we know that there are no overlapping
      // slices, or if there is only one slice to check anyway.
      if (!entry->data.overlaps(slice))
        return DxvkAccessFlags();

      ListEntry* list = getListEntry(entry->next);

      if (!list)
        return entry->data.getAccess();

      // The early out condition just checks whether there are
      // any access flags left that may potentially get added
      DxvkAccessFlags access;

      while (list && access != entry->data.getAccess()) {
        if (list->data.overlaps(slice))
          access.set(list->data.getAccess());

        list = getListEntry(list->next);
      }

      return access;
    }

    /**
     * \brief Checks whether a given resource slice is dirty
     *
     * \param [in] resource Resourece handle
     * \param [in] slice Resource slice
     * \returns \c true if there is at least one slice that
     *    overlaps with the given slice, and either slice has
     *    the \c DxvkAccess::Write flag set.
     */
    bool isDirty(K resource, const T& slice) {
      HashEntry* entry = findHashEntry(resource);

      if (!entry)
        return false;

      // Exit early if there are no overlapping slices, or
      // if none of the slices have the write flag set.
      if (!entry->data.isDirty(slice))
        return false;

      // We know that some subresources are dirty, so if
      // there is no list, the given slice must be dirty.
      ListEntry* list = getListEntry(entry->next);

      if (!list)
        return true;

      // Exit earlier if we find one dirty slice
      bool dirty = false;

      while (list && !dirty) {
        dirty = list->data.isDirty(slice);
        list = getListEntry(list->next);
      }

      return dirty;
    }

    /**
     * \brief Inserts a given resource slice
     *
     * This will attempt to deduplicate and merge entries if
     * possible, so that lookup and further insertions remain
     * reasonably fast.
     * \param [in] resource Resource handle
     * \param [in] slice Resource slice
     */
    void insert(K resource, const T& slice) {
      HashEntry* hashEntry = insertHashEntry(resource, slice);

      if (hashEntry) {
        ListEntry* listEntry = getListEntry(hashEntry->next);

        if (listEntry) {
          if (std::is_same_v<T, DxvkBarrierImageSlice>) {
            // For images, try to merge the slice with existing
            // entries if possible to keep the list small
            do {
              if (listEntry->data.canMerge(slice)) {
                listEntry->data.merge(slice);
                break;
              }
            } while ((listEntry = getListEntry(listEntry->next)));

            if (!listEntry)
              insertListEntry(slice, hashEntry);
          } else {
            // For buffers it's not even worth trying. Most of the
            // time we won't be able to merge, and traversing the
            // entire list every time is slow.
            insertListEntry(slice, hashEntry);
          }
        } else if (!hashEntry->data.canMerge(slice)) {
          // Only create the linear list if absolutely necessary
          insertListEntry(hashEntry->data, hashEntry);
          insertListEntry(slice, hashEntry);
        }

        // Merge hash entry data so that it stores
        // a superset of all slices in the list.
        hashEntry->data.merge(slice);
      }
    }

    /**
     * \brief Removes all resources from the set
     */
    void clear() {
      m_used = 0;
      m_version += 1;
      m_list.clear();
    }

    /**
     * \brief Checks whether set is empty
     * \returns \c true if there are no entries
     */
    bool empty() const {
      return m_used == 0;
    }

  private:

    struct ListEntry {
      T         data;
      uint32_t  next;
    };

    struct HashEntry {
      uint64_t  version;
      K         key;
      T         data;
      uint32_t  next;
    };

    uint64_t m_version = 1ull;
    uint64_t m_used    = 0ull;
    size_t   m_indexMask = 0;

    std::vector<ListEntry> m_list;
    std::vector<HashEntry> m_hashMap;

    static size_t computeHash(K key) {
      size_t hash = size_t(key) * 93887;
      return hash ^ (hash >> 16);
    }

    size_t computeSize() const {
      return m_indexMask
        ? m_indexMask + 1
        : 0;
    }

    size_t computeIndex(K key) const {
      return computeHash(key) & (m_indexMask);
    }

    size_t advanceIndex(size_t index) const {
      return (index + 1) & m_indexMask;
    }

    HashEntry* findHashEntry(K key) {
      if (!m_used)
        return nullptr;

      size_t index = computeIndex(key);

      while (m_hashMap[index].version == m_version) {
        if (m_hashMap[index].key == key)
          return &m_hashMap[index];

        index = advanceIndex(index);
      }

      return nullptr;
    }

    HashEntry* insertHashEntry(K key, const T& data) {
      growHashMapBeforeInsert();

      // If we already have an entry for the given key, return
      // the old one and let the caller deal with it
      size_t index = computeIndex(key);

      while (m_hashMap[index].version == m_version) {
        if (m_hashMap[index].key == key)
          return &m_hashMap[index];

        index = advanceIndex(index);
      }

      HashEntry* entry = &m_hashMap[index];
      entry->version = m_version;
      entry->key     = key;
      entry->data    = data;
      entry->next    = NoEntry;

      m_used += 1;
      return nullptr;
    }

    void growHashMap(size_t newSize) {
      size_t oldSize = computeSize();
      m_hashMap.resize(newSize);

      // Relocate hash entries in place
      for (size_t i = 0; i < oldSize; i++) {
        HashEntry entry = m_hashMap[i];
        m_hashMap[i].version = 0;

        while (entry.version == m_version) {
          size_t index = computeIndex(entry.key);
          entry.version = m_version + 1;

          while (m_hashMap[index].version > m_version)
            index = advanceIndex(index);

          std::swap(entry, m_hashMap[index]);
        }
      }

      m_version += 1;
      m_indexMask = newSize - 1;
    }

    void growHashMapBeforeInsert() {
      // Allow a load factor of 0.7 for performance reasons
      size_t oldSize = computeSize();

      if (10 * m_used >= 7 * oldSize) {
        size_t newSize = oldSize ? oldSize * 2 : 64;
        growHashMap(newSize);
      }
    }

    ListEntry* getListEntry(uint32_t index) {
      return index < NoEntry ? &m_list[index] : nullptr;
    }

    ListEntry* insertListEntry(const T& subresource, HashEntry* head) {
      uint32_t newIndex = uint32_t(m_list.size());
      m_list.push_back({ subresource, head->next });
      head->next = newIndex;
      return &m_list[newIndex];
    }

  };
  
  /**
   * \brief Barrier set
   * 
   * Accumulates memory barriers and provides a
   * method to record all those barriers into a
   * command buffer at once.
   */
  class DxvkBarrierSet {
    
  public:
    
    DxvkBarrierSet(DxvkCmdBuffer cmdBuffer);
    ~DxvkBarrierSet();

    void accessMemory(
            VkPipelineStageFlags      srcStages,
            VkAccessFlags             srcAccess,
            VkPipelineStageFlags      dstStages,
            VkAccessFlags             dstAccess);

    void accessBuffer(
      const DxvkBufferSliceHandle&    bufSlice,
            VkPipelineStageFlags      srcStages,
            VkAccessFlags             srcAccess,
            VkPipelineStageFlags      dstStages,
            VkAccessFlags             dstAccess);
    
    void accessImage(
      const Rc<DxvkImage>&            image,
      const VkImageSubresourceRange&  subresources,
            VkImageLayout             srcLayout,
            VkPipelineStageFlags      srcStages,
            VkAccessFlags             srcAccess,
            VkImageLayout             dstLayout,
            VkPipelineStageFlags      dstStages,
            VkAccessFlags             dstAccess);

    void releaseImage(
            DxvkBarrierSet&           acquire,
      const Rc<DxvkImage>&            image,
      const VkImageSubresourceRange&  subresources,
            uint32_t                  srcQueue,
            VkImageLayout             srcLayout,
            VkPipelineStageFlags      srcStages,
            VkAccessFlags             srcAccess,
            uint32_t                  dstQueue,
            VkImageLayout             dstLayout,
            VkPipelineStageFlags      dstStages,
            VkAccessFlags             dstAccess);
    
    bool isBufferDirty(
      const DxvkBufferSliceHandle&    bufSlice,
            DxvkAccessFlags           bufAccess);

    bool isImageDirty(
      const Rc<DxvkImage>&            image,
      const VkImageSubresourceRange&  imgSubres,
            DxvkAccessFlags           imgAccess);
    
    DxvkAccessFlags getBufferAccess(
      const DxvkBufferSliceHandle&    bufSlice);
    
    DxvkAccessFlags getImageAccess(
      const Rc<DxvkImage>&            image,
      const VkImageSubresourceRange&  imgSubres);
    
    VkPipelineStageFlags getSrcStages() {
      return m_allBarrierSrcStages;
    }
    
    void finalize(
      const Rc<DxvkCommandList>&      commandList);

    void recordCommands(
      const Rc<DxvkCommandList>&      commandList);
    
    void reset();

    bool hasResourceBarriers() const {
      return !m_bufSlices.empty()
          || !m_imgSlices.empty();
    }

    static DxvkAccessFlags getAccessTypes(VkAccessFlags flags);
    
  private:

    struct BufSlice {
      DxvkBufferSliceHandle   slice;
      DxvkAccessFlags         access;
    };

    struct ImgSlice {
      VkImage                 image;
      VkImageSubresourceRange subres;
      DxvkAccessFlags         access;
    };

    DxvkCmdBuffer m_cmdBuffer;

    VkPipelineStageFlags2 m_hostBarrierSrcStages = 0;
    VkAccessFlags2        m_hostBarrierDstAccess = 0;

    VkPipelineStageFlags2 m_allBarrierSrcStages = 0;

    VkMemoryBarrier2 m_memBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    std::vector<VkImageMemoryBarrier2>  m_imgBarriers;

    DxvkBarrierSubresourceSet<VkBuffer, DxvkBarrierBufferSlice> m_bufSlices;
    DxvkBarrierSubresourceSet<VkImage,  DxvkBarrierImageSlice>  m_imgSlices;
    
  };
  
}