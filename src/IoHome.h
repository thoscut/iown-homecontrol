/**
 * @file IoHome.h
 * @brief Thin RadioLib-oriented io-homecontrol node (legacy API)
 *
 * This is the original, low-level entry point of the project. It only owns the
 * physical layer configuration and a few byte-order helpers; frame building,
 * authentication and the receive policy live in IoHomeControl, which is what
 * new code should use.
 *
 * It is kept because existing sketches include it and because it is the
 * smallest possible starting point for experiments that drive the radio
 * directly.
 */

#if !defined(_IOHOME_H)
#define _IOHOME_H

#include <RadioLib.h>
#include <stddef.h>
#include <stdint.h>

#pragma region CONST
// preamble
#define IOHOME_PREAMBLE_LEN                 (512) // Preamble length in bits

// sync word (already bit-reversed for MSB-first radios)
#define IOHOME_SYNC_WORD                    (0x57FD99)
#define IOHOME_SYNC_WORD_LEN                (3)

// Control Byte 0 layout (see docs/linklayer.md):
//   bits 7-6 = Order, bit 5 = isOneWay, bits 4-0 = Size
#define IOHOME_CTRLBYTE0_MODE_TWOWAY        (0x00 << 5)        //  5     5
#define IOHOME_CTRLBYTE0_MODE_ONEWAY        (0x01 << 5)        //  5     5
#define IOHOME_CTRLBYTE0_ORDER_0            (0x00 << 6)        //  7     6
#define IOHOME_CTRLBYTE0_ORDER_1            (0x01 << 6)        //  7     6
#define IOHOME_CTRLBYTE0_ORDER_2            (0x02 << 6)        //  7     6
#define IOHOME_CTRLBYTE0_ORDER_3            (0x03 << 6)        //  7     6

// `Size` is the frame length excluding Control Byte 0 and the CRC.
#define IOHOME_CTRLBYTE0_LENGTH(IOHOME_LEN) ((IOHOME_LEN) & 0x1F)  //  4     0
#define IOHOME_FRAME_LEN(IOHOME_SIZE)       ((IOHOME_SIZE) + 3)

// frame header layout
#define IOHOME_CTRLBYTE0_POS                (0x0)
#define IOHOME_CTRLBYTE1_POS                (0x1)
#define IOHOME_MAC_DEST_POS                 (0x2)
#define IOHOME_MAC_SOURCE_POS               (0x5)
#define IOHOME_CMD_POS                      (0x8)

#define IOHOME_NUM_COMMANDS                 (2)
#define IOHOME_CMD_0x00                     (0x00)
#define IOHOME_CMD_0x01                     (0x01)
#pragma endregion CONST

/*!
  \struct IoHomeCommands_t
  \brief Command specification structure.
*/
struct IoHomeCommands_t {
  /*! \brief Command ID */
  uint8_t cid;

  /*! \brief Payload length in bytes, excluding the command ID */
  uint8_t len;

  /*! \brief Number of parameters */
  uint8_t pnum;

  /*! \brief Whether this command needs authentication */
  bool auth;
};

// Commands 0x00 and 0x01 both carry originator + ACEI + a 2-byte parameter
// plus 2 further bytes = 6 payload bytes, and both are authenticated in 1W.
const IoHomeCommands_t CMD_TABLE[IOHOME_NUM_COMMANDS] = {
  { IOHOME_CMD_0x00, 6, 1, true },
  { IOHOME_CMD_0x01, 6, 1, true },
};

struct NodeId {
  uint8_t n0;
  uint8_t n1;
  uint8_t n2;
};

struct IoHomeChannel_t {
  uint8_t c0;
  uint8_t c1;
};

/*!
  \class IoHomeNode
  \brief io-homecontrol node.
*/
class IoHomeNode {
  public:

    /*!
      \brief Default constructor.
      \param phy Pointer to the PhysicalLayer radio module.
      \param channel Pointer to the io-homecontrol channel to use.
    */
    explicit IoHomeNode(PhysicalLayer* phy, const IoHomeChannel_t* channel = nullptr);

    /*!
      \brief Store the node configuration.
      \param chan Channel to operate on.
      \param source_node_id This node's address.
      \param destination_node_id Default peer address.
      \param stack_key 16-byte stack key, or nullptr.
      \param system_key 16-byte system key, or nullptr.
      \returns true when the parameters were accepted.
    */
    bool begin(const IoHomeChannel_t* chan,
               NodeId source_node_id,
               NodeId destination_node_id,
               const uint8_t* stack_key = nullptr,
               const uint8_t* system_key = nullptr);

    PhysicalLayer* phyLayer = nullptr;
    const IoHomeChannel_t* channel = nullptr;

    /*! \brief Configure common physical layer properties (preamble, sync word, ...). */
    int16_t setPhyProperties();

    /*!
      \brief CRC-16/KERMIT over a buffer, as used by io-homecontrol frames.
      \param data Buffer to checksum.
      \param len Number of bytes.
      \returns The CRC; transmit the least significant byte first.
    */
    static uint16_t crc16(const uint8_t* data, size_t len);

    /*!
      \brief Network-to-host conversion.

      io-homecontrol multi-byte fields are little-endian on the wire.

      \note Defined in the header: a template body in the .cpp would not be
            visible to callers and every use would fail to link.
    */
    template<typename T>
    static T ntoh(const uint8_t* buff, size_t size = 0) {
      if (buff == nullptr) {
        return static_cast<T>(0);
      }
      size_t targetSize = (size != 0) ? size : sizeof(T);
      if (targetSize > sizeof(T)) {
        targetSize = sizeof(T);
      }
      T res = 0;
      for (size_t i = 0; i < targetSize; i++) {
        res = static_cast<T>(res | (static_cast<T>(buff[i]) << (8 * i)));
      }
      return res;
    }

    /*! \brief Host-to-network conversion. */
    template<typename T>
    static void hton(uint8_t* buff, T val, size_t size = 0) {
      if (buff == nullptr) {
        return;
      }
      size_t targetSize = (size != 0) ? size : sizeof(T);
      if (targetSize > sizeof(T)) {
        targetSize = sizeof(T);
      }
      for (size_t i = 0; i < targetSize; i++) {
        buff[i] = static_cast<uint8_t>(val >> (8 * i));
      }
    }

    NodeId sourceNodeId = {0, 0, 0};
    NodeId destinationNodeId = {0, 0, 0};

    bool hasStackKey() const { return stackKeySet_; }
    bool hasSystemKey() const { return systemKeySet_; }

  private:
    uint8_t stackKey_[16] = {0};
    uint8_t systemKey_[16] = {0};
    bool stackKeySet_ = false;
    bool systemKeySet_ = false;
};

#endif
