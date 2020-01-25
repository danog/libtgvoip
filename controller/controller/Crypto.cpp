#include "../../VoIPController.h"

using namespace tgvoip;
using namespace std;

void VoIPController::SetEncryptionKey(std::vector<uint8_t> key, bool isOutgoing)
{
    memcpy(encryptionKey, key.data(), 256);
    uint8_t sha1[SHA1_LENGTH];
    crypto.sha1((uint8_t *)encryptionKey, 256, sha1);
    memcpy(keyFingerprint, sha1 + (SHA1_LENGTH - 8), 8);
    uint8_t sha256[SHA256_LENGTH];
    crypto.sha256((uint8_t *)encryptionKey, 256, sha256);
    memcpy(callID, sha256 + (SHA256_LENGTH - 16), 16);
    this->isOutgoing = isOutgoing;
}

void VoIPController::KDF(unsigned char *msgKey, size_t x, unsigned char *aesKey, unsigned char *aesIv)
{
    uint8_t sA[SHA1_LENGTH], sB[SHA1_LENGTH], sC[SHA1_LENGTH], sD[SHA1_LENGTH];
    BufferOutputStream buf(128);
    buf.WriteBytes(msgKey, 16);
    buf.WriteBytes(encryptionKey + x, 32);
    crypto.sha1(buf.GetBuffer(), buf.GetLength(), sA);
    buf.Reset();
    buf.WriteBytes(encryptionKey + 32 + x, 16);
    buf.WriteBytes(msgKey, 16);
    buf.WriteBytes(encryptionKey + 48 + x, 16);
    crypto.sha1(buf.GetBuffer(), buf.GetLength(), sB);
    buf.Reset();
    buf.WriteBytes(encryptionKey + 64 + x, 32);
    buf.WriteBytes(msgKey, 16);
    crypto.sha1(buf.GetBuffer(), buf.GetLength(), sC);
    buf.Reset();
    buf.WriteBytes(msgKey, 16);
    buf.WriteBytes(encryptionKey + 96 + x, 32);
    crypto.sha1(buf.GetBuffer(), buf.GetLength(), sD);
    buf.Reset();
    buf.WriteBytes(sA, 8);
    buf.WriteBytes(sB + 8, 12);
    buf.WriteBytes(sC + 4, 12);
    assert(buf.GetLength() == 32);
    memcpy(aesKey, buf.GetBuffer(), 32);
    buf.Reset();
    buf.WriteBytes(sA + 8, 12);
    buf.WriteBytes(sB, 8);
    buf.WriteBytes(sC + 16, 4);
    buf.WriteBytes(sD, 8);
    assert(buf.GetLength() == 32);
    memcpy(aesIv, buf.GetBuffer(), 32);
}

void VoIPController::KDF2(unsigned char *msgKey, size_t x, unsigned char *aesKey, unsigned char *aesIv)
{
    uint8_t sA[32], sB[32];
    BufferOutputStream buf(128);
    buf.WriteBytes(msgKey, 16);
    buf.WriteBytes(encryptionKey + x, 36);
    crypto.sha256(buf.GetBuffer(), buf.GetLength(), sA);
    buf.Reset();
    buf.WriteBytes(encryptionKey + 40 + x, 36);
    buf.WriteBytes(msgKey, 16);
    crypto.sha256(buf.GetBuffer(), buf.GetLength(), sB);
    buf.Reset();
    buf.WriteBytes(sA, 8);
    buf.WriteBytes(sB + 8, 16);
    buf.WriteBytes(sA + 24, 8);
    memcpy(aesKey, buf.GetBuffer(), 32);
    buf.Reset();
    buf.WriteBytes(sB, 8);
    buf.WriteBytes(sA + 8, 16);
    buf.WriteBytes(sB + 24, 8);
    memcpy(aesIv, buf.GetBuffer(), 32);
}