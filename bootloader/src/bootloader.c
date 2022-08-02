// Hardware Imports
#include "inc/hw_memmap.h" // Peripheral Base Addresses
#include "inc/lm3s6965.h"  // Peripheral Bit Masks and Registers
#include "inc/hw_types.h"  // Boolean type
#include "inc/hw_ints.h"   // Interrupt numbers

// Driver API Imports
#include "driverlib/flash.h"     // FLASH API
#include "driverlib/sysctl.h"    // System control API (clock/reset)
#include "driverlib/interrupt.h" // Interrupt API
#include "bearssl.h"

// Library Imports
#include <string.h>

// Application Imports
#include "uart.h"

// Forward Declarations
void load_initial_firmware(void);
void load_firmware(void);
void boot_firmware(void);
long program_flash(uint32_t, unsigned char *, unsigned int);
void read_frame(uint8_t uart_num, uint8_t *data);
int aes_gcm_decrypt_and_verify(unsigned char *key, unsigned char *nonce, unsigned char *ct, int ct_len, unsigned char *tag);
void reject();

// Firmware Constants
#define METADATA_BASE 0xFC00 // base address of version and firmware size in Flash
#define FW_BASE 0x10000      // base address of firmware in Flash
//#define FW_MEM_BASE 0x100000 // base address of firmware in RAM

// FLASH Constants
#define FLASH_PAGESIZE 1024
#define FLASH_WRITESIZE 4

// Protocol Constants
#define OK ((unsigned char)0x00)
#define ERROR ((unsigned char)0x01)
#define UPDATE ((unsigned char)'U')
#define BOOT ((unsigned char)'B')

// Size constants
#define MAX_FIRMWARE_SIZE 32768 // max firmware size of 32768 bytes
#define AES_KEY_LENGTH 16
#define V_KEY_LENGTH 64
#define ECC_KEY_LENGTH 65

#define FRAME_LENGTH 64

#include "secret.h"

#include "beaverssl.h"
// Keys
// unsigned char AES_KEY[AES_KEY_LENGTH] = AES;
// unsigned char V_KEY[V_KEY_LENGTH] = VIG;
// unsigned char ECC_KEY[ECC_KEY_LENGTH] = ECC;

// Firmware v2 is embedded in bootloader
// Read up on these symbols in the objcopy man page (if you want)!
extern int _binary_firmware_bin_start;
extern int _binary_firmware_bin_size;

// Device metadata
uint16_t *fw_version_address = (uint16_t *)METADATA_BASE;
uint16_t *fw_size_address = (uint16_t *)(METADATA_BASE + 2);
uint8_t *fw_release_message_address;

// Firmware Buffer
// unsigned char buffer[FLASH_PAGESIZE];

// define the ECC public key
static const br_ec_public_key ECC_PUB_KEY = {.curve = BR_EC_secp256r1, .q = (void *)ECC_KEY, .qlen = sizeof(ECC_KEY)};

int main(void)
{

  // A 'reset' on UART0 will re-start this code at the top of main, won't clear flash, but will clean ram.

  // Initialize UART channels
  // 0: Reset
  // 1: Host Connection
  // 2: Debug
  uart_init(UART0);
  uart_init(UART1);
  uart_init(UART2);

  // Enable UART0 interrupt
  IntEnable(INT_UART0);
  IntMasterEnable();

  load_initial_firmware(); // note the short-circuit behavior in this function, it doesn't finish running on reset!

  uart_write_str(UART2, "Welcome to the BWSI Vehicle Update Service!\n");
  uart_write_str(UART2, "Send \"U\" to update, and \"B\" to run the firmware.\n");
  uart_write_str(UART2, "Writing 0x20 to UART0 will reset the device.\n");

  int resp;
  while (1)
  {
    uint32_t instruction = uart_read(UART1, BLOCKING, &resp);
    if (instruction == UPDATE)
    {
      uart_write_str(UART1, "U");
      load_firmware();
    }
    else if (instruction == BOOT)
    {
      uart_write_str(UART1, "B");
      boot_firmware();
    }
  }
}

/*
 * Load initial firmware into flash
 */
void load_initial_firmware(void)
{

  if (*((uint32_t *)(METADATA_BASE)) != 0xFFFFFFFF)
  {
    /*
     * Default Flash startup state is all FF since. Only load initial
     * firmware when metadata page is all FF. Thus, exit if there has
     * been a reset!
     */
    return;
  }

  // Create buffers for saving the release message
  uint8_t temp_buf[FLASH_PAGESIZE];
  char initial_msg[] = "This is the initial release message.";
  uint16_t msg_len = strlen(initial_msg) + 1;
  uint16_t rem_msg_bytes;

  // Get included initial firmware
  int size = (int)&_binary_firmware_bin_size;
  uint8_t *initial_data = (uint8_t *)&_binary_firmware_bin_start;

  // Set version 2 and install
  uint16_t version = 2;
  uint32_t metadata = (((uint16_t)size & 0xFFFF) << 16) | (version & 0xFFFF);
  program_flash(METADATA_BASE, (uint8_t *)(&metadata), 4);

  int i;

  for (i = 0; i < size / FLASH_PAGESIZE; i++)
  {
    program_flash(FW_BASE + (i * FLASH_PAGESIZE), initial_data + (i * FLASH_PAGESIZE), FLASH_PAGESIZE);
  }

  /* At end of firmware. Since the last page may be incomplete, we copy the initial
   * release message into the unused space in the last page. If the firmware fully
   * uses the last page, the release message simply is written to a new page.
   */

  uint16_t rem_fw_bytes = size % FLASH_PAGESIZE;
  if (rem_fw_bytes == 0)
  {
    // No firmware left. Just write the release message
    program_flash(FW_BASE + (i * FLASH_PAGESIZE), (uint8_t *)initial_msg, msg_len);
  }
  else
  {
    // Some firmware left. Determine how many bytes of release message can fit
    if (msg_len > (FLASH_PAGESIZE - rem_fw_bytes))
    {
      rem_msg_bytes = msg_len - (FLASH_PAGESIZE - rem_fw_bytes);
    }
    else
    {
      rem_msg_bytes = 0;
    }

    // Copy rest of firmware
    memcpy(temp_buf, initial_data + (i * FLASH_PAGESIZE), rem_fw_bytes);
    // Copy what will fit of the release message
    memcpy(temp_buf + rem_fw_bytes, initial_msg, msg_len - rem_msg_bytes);
    // Program the final firmware and first part of the release message
    program_flash(FW_BASE + (i * FLASH_PAGESIZE), temp_buf, rem_fw_bytes + (msg_len - rem_msg_bytes));

    if (rem_msg_bytes > 0)
    {
      // If there are more bytes, program them directly from the release message string
      // Writing to a new page. Increment pointer
      i++;
      program_flash(FW_BASE + (i * FLASH_PAGESIZE), (uint8_t *)(initial_msg + (msg_len - rem_msg_bytes)), rem_msg_bytes);
    }
  }
}

// read a 64 byte frame of data from specified UART interface
void read_frame(uint8_t uart_num, uint8_t *data)
{
  uint32_t instruction;
  int resp;
  uint8_t i;
  for (i = 0; i < FRAME_LENGTH; i++)
  {
    instruction = uart_read(uart_num, BLOCKING, &resp);
    data[i] = instruction;
    // uart_write_hex(UART2, data[i]);
  }
  uart_write(UART1, OK); // tell client we received the frame
  uart_write_str(UART2, "\nOK signal sent back\n");
}

void reject()
{
  uart_write(UART1, ERROR); // tell client there is error
  SysCtlReset();            // Reset device
}
/*
 * Load the firmware into flash.
 */
void load_firmware(void)
{
  int frame_length = 0;
  int read = 0;
  uint32_t rcv = 0;

  uint32_t page_addr = FW_BASE;
  uint32_t version = 0;
  uint16_t old_version = *fw_version_address;

  // define the storage pointer for the frame data
  uint8_t *sp = (uint8_t *)0x100000;
  uint8_t *ecc_signature;
  ecc_signature = 0x100000;

  uint8_t *bigArray = (uint8_t *)0x20000500;

  // write the AES_KEY to UART2
  uart_write_str(UART2, "AES_KEY: \n");
  for (int i = 0; i < AES_KEY_LENGTH; i++)
  {
    uart_write_hex(UART2, AES_KEY[i]);
  }

  // Read the first packet of data(16 bytes of auth tag, 16 bytes of nonce)
  uint8_t auth_tag[16];
  uint8_t nonce[16];
  for (int i = 0; i < 16; i++)
  {
    auth_tag[i] = uart_read(UART1, BLOCKING, &read);
  }
  for (int i = 0; i < 16; i++)
  {
    nonce[i] = uart_read(UART1, BLOCKING, &read);
  }

  uint8_t frame_counter = 0;
  uint8_t frame_data[FRAME_LENGTH];
  // loops until data array becomes 64 null bytes
  while (frame_counter * FRAME_LENGTH < MAX_FIRMWARE_SIZE)
  {
    // read 64 bytes of data from UART1
    read_frame(UART1, frame_data);
    uart_write_hex(UART2, frame_counter);

    // if data is all null bytes, break loop
    // uncursed version
    int found_nonzero_byte = 0;

    for (int i = 0; i < 64; i++)
    {
      // prints the data to the UART2
      uart_write_hex(UART2, frame_data[i]);
      // checks if there is a non-zero byte
      if (frame_data[i] != 0)
      {
        found_nonzero_byte = 1;
        break;
      }
    }
    // stops if frame is all null bytes
    if (!found_nonzero_byte)
    {
      uart_write_str(UART2, "All null bytes.\n");
      break;
    }

    uart_write_str(UART2, "\n");

    // copy the data to the buffer array at index frame_counter * FRAME_LENGTH from frame_data
    for (int i = 0; i < FRAME_LENGTH; i++)
    {
      bigArray[frame_counter * FRAME_LENGTH + i] = frame_data[i];
      // uart_write_hex(UART2, bigArray[frame_counter * FRAME_LENGTH + i]);
      // uart_write_hex(UART2, frame_counter * FRAME_LENGTH + i);
      // uart_write_str(UART2, "\n");
    }

    // increment the frame counter this is put afterwards so last frame isn't counted as a data frame even though null frame is written
    frame_counter += 1;
    uart_write_hex(UART2, frame_counter);
  }

  // Compare to old version and abort if older (note special case for version 0).

  uart_write(UART1, OK); // Acknowledge the metadata.

  // Decrypt and verify

  uart_write_str(UART2, "\nVigenere Decrypting...\n");

  // Vignere decryption
  for (int i = 0; i < FRAME_LENGTH * frame_counter; i++)
  {
    bigArray[i] = V_KEY[i % FRAME_LENGTH] ^ bigArray[i];
    uart_write_hex(UART2, bigArray[i]);
  }
  // not a while loop for accidental nulls

  // // copy buffer data to new array
  // unsigned char new_buffer[FRAME_LENGTH * frame_counter];
  // for (int i = 0; i < FRAME_LENGTH * frame_counter; i++)
  // {
  //   uart_write_hex(UART2, bigArray[i]);
  // }

  uart_write_str(UART2, "\nAES Decrypting...\n");

  unsigned char aad[16]; // Empty char array bc we're not using AAD
  // GCM decrypt
  if (!(gcm_decrypt_and_verify(AES_KEY, *nonce, bigArray, (frame_counter)*FRAME_LENGTH, aad, 16, *auth_tag))) // this prolly won't work
                                                                                                              // first frame is tag and nonce so should be excluded
  {
    reject();
    return;
  }

  // data_no_signature points to the start of the data without the ECC signature in the buffer
  uint8_t *data_no_signature = bigArray + 64;

  uart_write_str(UART2, "\nECC Verifying...\n");

  // Hash data
  unsigned char hashed_data[32];
  sha_hash(*data_no_signature, frame_counter * FRAME_LENGTH - FRAME_LENGTH, hashed_data);

  // Verify ECC signature
  if (br_ecdsa_i31_vrfy_asn1(&br_ec_p256_m31, hashed_data, 32, &ECC_PUB_KEY, bigArray, 64) != 1)
  {
    reject();
    return;
  }

  version = *(sp + 64) | *(sp + 64 + 1) << 8;
  if (version != 0 && version < old_version)
  {
    reject();
    return;
  }
  else if (version == 0)
  {
    // If debug firmware, don't change version
    version = old_version;
  }

  // Write new firmware size and version to Flash
  uint16_t fw_size = *(sp + 64 + 2) | *(sp + 64 + 3) << 8;
  // Create 32 bit word for flash programming, version is at lower address, size is at higher address
  program_flash(METADATA_BASE, (uint8_t *)version, 2);
  program_flash(METADATA_BASE, (uint8_t *)fw_size, 2);

  // Flash everything in memory
  int i = 0;
  for (; i < fw_size; i++)
  {
    program_flash(FW_BASE, (uint8_t *)sp + FRAME_LENGTH + 4 + i, 1);
  }
  // Write debugging messages to UART2.
  uart_write_str(UART2, "Firmware successfully programmed\nAddress: ");
  uart_write_hex(UART2, sp + FRAME_LENGTH + 4 + i);
  uart_write_str(UART2, "\nBytes: ");
  uart_write_hex(UART2, i);
  nl(UART2);
}

/*
 * Program a stream of bytes to the flash.
 * This function takes the starting address of a 1KB page, a pointer to the
 * data to write, and the number of byets to write.
 *
 * This functions performs an erase of the specified flash page before writing
 * the data.
 */
long program_flash(uint32_t page_addr, unsigned char *data, unsigned int data_len)
{
  uint32_t word = 0;
  int ret;
  int i;

  // Erase next FLASH page
  FlashErase(page_addr);

  // Clear potentially unused bytes in last word
  // If data not a multiple of 4 (word size), program up to the last word
  // Then create temporary variable to create a full last word
  if (data_len % FLASH_WRITESIZE)
  {
    // Get number of unused bytes
    int rem = data_len % FLASH_WRITESIZE;
    int num_full_bytes = data_len - rem;

    // Program up to the last word
    ret = FlashProgram((unsigned long *)data, page_addr, num_full_bytes);
    if (ret != 0)
    {
      return ret;
    }

    // Create last word variable -- fill unused with 0xFF
    for (i = 0; i < rem; i++)
    {
      word = (word >> 8) | (data[num_full_bytes + i] << 24); // Essentially a shift register from MSB->LSB
    }
    for (i = i; i < 4; i++)
    {
      word = (word >> 8) | 0xFF000000;
    }

    // Program word
    return FlashProgram(&word, page_addr + num_full_bytes, 4);
  }
  else
  {
    // Write full buffer of 4-byte words
    return FlashProgram((unsigned long *)data, page_addr, data_len);
  }
}

void boot_firmware(void)
{
  // compute the release message address, and then print it
  uint16_t fw_size = *fw_size_address;
  fw_release_message_address = (uint8_t *)(FW_BASE + fw_size);
  uart_write_str(UART2, (char *)fw_release_message_address);

  // Boot the firmware
  __asm(
      "LDR R0,=0x10001\n\t"
      "BX R0\n\t");
}
// /*
//  * AES-128 GCM Decrypt and Verify
//  */
// int aes_gcm_decrypt_and_verify(unsigned char *key, unsigned char *nonce, unsigned char *ct, int ct_len, unsigned char *tag)
// {
//   // write the AES_KEY to UART2
//   uart_write_str(UART2, "AES_KEY: \n");
//   for (int i = 0; i < AES_KEY_LENGTH; i++)
//   {
//     uart_write_hex(UART2, AES_KEY[i]);
//   }
//   br_aes_ct_ctr_keys bc;
//   br_gcm_context gc;
//   br_aes_ct_ctr_init(&bc, key, 16);
//   uart_write_str(UART2, "Decrypting1...\n");
//   br_gcm_init(&gc, &bc.vtable, br_ghash_ctmul32);
//   uart_write_str(UART2, "Decrypting2...\n");

//   br_gcm_reset(&gc, nonce, 16);
//   uart_write_str(UART2, "Decrypting3...\n");
//   br_gcm_flip(&gc);
//   uart_write_str(UART2, "Decrypting...\n");
//   br_gcm_run(&gc, 0, ct, ct_len);
//   if (br_gcm_check_tag(&gc, tag))
//   {
//     return 1;
//   }
//   return 0;
// }