"""
Firmware Bundle-and-Protect Tool

"""
import argparse
import struct
from Crypto.Cipher import AES
from Crypto.PublicKey import ECC
from Crypto.Signature import eddsa

def protect_firmware(infile, outfile, version, message):
    # Load firmware binary from infile
    with open(infile, 'rb') as fp:
        firmware = fp.read()
        
    # load secret keys from file
    with open('secret_build_output.txt', 'rb') as secrets_file:
        aes_key = secrets_file.read(16)
        priv_key = secrets_file.read(48) 
        pub_key = secrets_file.read(44)
        vkey = secrets_file.read(16)

    # Append null-terminated message to end of firmware
    firmware_and_message = firmware + message.encode() + b'\00'

    # Pack version and size into two little-endian shorts
    metadata = struct.pack('<HH', version, len(firmware))

    # Append firmware and message to metadata
    firmware_blob = metadata + firmware_and_message
    
    
    # vigenere cipher
    
    # pad the vkey to fit all of the data
    vkey *= len(firmware)//len(vkey)
    vkey += vkey[:len(firmware)%len(vkey)]
    
    firmware_blob = bytes(a ^ b for a, b in zip(firmware_blob, vkey))

    # sign
    key = ECC.import_key(open('secret_build_output.txt').read())
    signer = eddsa.new(key, 'rfc8032')
    
    
    # Create cipher object
    cipher = AES.new(aes_key, AES.MODE_GCM)
    # assert that version is an integer
    cipher.update(version)
    

    nonce = cipher.nonce
    
    encrypted_firmware_blob, tag = cipher.encrypt_and_digest(firmware_blob)
    
    

    # Write firmware blob to outfile
    with open(outfile, 'wb+') as outfile:
        outfile.write(encrypted_firmware_blob + tag + nonce)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Firmware Update Tool')
    parser.add_argument("--infile", help="Path to the firmware image to protect.", required=True)
    parser.add_argument("--outfile", help="Filename for the output firmware.", required=True)
    parser.add_argument("--version", help="Version number of this firmware.", required=True)
    parser.add_argument("--message", help="Release message for this firmware.", required=True)
    args = parser.parse_args()

    protect_firmware(infile=args.infile, outfile=args.outfile, version=int(args.version), message=args.message)
