#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <stdint.h>

#define SECTOR_SIZE 512
#define BUFFER_SIZE (16 * 1024 * 1024)

#pragma pack(push, 1)
typedef struct {
    BYTE bootIndicator;
    BYTE startHead;
    BYTE startSector;
    BYTE startCylinder;
    BYTE systemID;
    BYTE endHead;
    BYTE endSector;
    BYTE endCylinder;
    DWORD StartingLBA;
    DWORD totalSectors;
} PARTITION_ENTRY;

typedef struct {
    BYTE bootCode[446];
    PARTITION_ENTRY partitions[4];
    WORD signature; // 0xAA55
} MBR;

typedef struct {
    BYTE bootCode[446];
    PARTITION_ENTRY partition;
    PARTITION_ENTRY nextPartition;
    WORD signature; // 0xAA55
} EBR;

const char* get_fs_type_mbr(BYTE systemID) {
    switch (systemID) {
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 (<32MB)";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS/exFAT";
        case 0x0B: return "FAT32";
        case 0x0C: return "FAT32 (LBA)";
        case 0x0E: return "FAT16 (LBA)";
        case 0x0F: return "Extended (LBA)";
        case 0xEE: return "GPT Protective MBR";
        default: return "Unknown";
    }
}
//================================================================================================================



void crtFullDiskImage(int diskNum, const char* outFile) {
    printf("\n--------------crtFullDiskImage----------------\n Disk=%d   %s\n", diskNum, outFile);
    //return;

   char diskPath[64];
    sprintf(diskPath, "\\\\.\\PhysicalDrive%d", diskNum);

    HANDLE hDisk = CreateFileA(diskPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hDisk == INVALID_HANDLE_VALUE) {
        printf("Failed to open disk %d. Error: %lu\n", diskNum, GetLastError());
        return;
    }

    // Get disk size
    GET_LENGTH_INFORMATION info;
    DWORD bytesReturned;
    if (!DeviceIoControl(hDisk, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &info, sizeof(info), &bytesReturned, NULL)) {
        printf("Failed to get disk size. Error: %lu\n", GetLastError());
        CloseHandle(hDisk);
        return;
    }
    ULONGLONG diskSize = info.Length.QuadPart;

    // Open output file
    FILE* f = fopen(outFile, "wb");
    if (!f) {
        printf("Failed to open output file %s. Error: %d\n", outFile, errno);
        CloseHandle(hDisk);
        return;
    }

    // Allocate buffer
    BYTE* buffer = (BYTE*)malloc(BUFFER_SIZE);
    if (!buffer) {
        printf("Memory allocation failed\n");
        fclose(f);
        CloseHandle(hDisk);
        return;
    }

    // Read and write data
    ULONGLONG totalRead = 0;
    DWORD bytesRead, bytesWritten;
    while (totalRead < diskSize) {
        ULONGLONG toRead = (diskSize - totalRead) > BUFFER_SIZE ? BUFFER_SIZE : (diskSize - totalRead);

        if (!ReadFile(hDisk, buffer, (DWORD)toRead, &bytesRead, NULL) || bytesRead == 0) {
            printf("Read error at offset %llu. Error: %lu\n", totalRead, GetLastError());
            free(buffer);
            fclose(f);
            CloseHandle(hDisk);
            return;
        }

        if (fwrite(buffer, 1, bytesRead, f) != bytesRead) {
            printf("Write error at offset %llu. Error: %d\n", totalRead, errno);
            free(buffer);
            fclose(f);
            CloseHandle(hDisk);
            return;
        }

        totalRead += bytesRead;
        printf("\rProgress: %.2f MB", totalRead / (1024.0 * 1024.0));
        fflush(stdout);     // Ensures correct progress output
    }

    // Free resources
    free(buffer);
    fclose(f);
    CloseHandle(hDisk);

    printf("\nImage created: %s (%.2f GB)\n", outFile, diskSize / (1024.0 * 1024 * 1024));
}




int crtPartImage(int driveNumber, int partitionNum, const char* outputPath) {
    printf("\n--------------crtPartImage----------------\n Disk=%d  Part=%d  %s\n", driveNumber, partitionNum, outputPath);

    char diskPath[64];
    sprintf(diskPath, "\\\\.\\PhysicalDrive%d", driveNumber);

    HANDLE hDrive = CreateFileA(diskPath,  GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,  NULL   );

    if (hDrive == INVALID_HANDLE_VALUE) {
        perror("Failed to open physical drive");
        return 1;
    }

    DWORD bytesRead;
    MBR mbr;

    if (!ReadFile(hDrive, &mbr, sizeof(MBR), &bytesRead, NULL)) {
        perror("Failed to read MBR");
        CloseHandle(hDrive);
        return 1;
    }

    if (mbr.signature != 0xAA55) {
        printf("Invalid MBR signature: 0x%04X\n", mbr.signature);
        CloseHandle(hDrive);
        return 1;
    }

    if (partitionNum < 0 || partitionNum > 3) {
        printf("Invalid partition number. Must be 0-3\n");
        CloseHandle(hDrive);
        return 1;
    }

    PARTITION_ENTRY partition = mbr.partitions[partitionNum];

    if (partition.totalSectors == 0) {
        printf("Selected partition is empty or not valid.\n");
        CloseHandle(hDrive);
        return 1;
    }

    LARGE_INTEGER partitionOffset;
    EBR ebr;
    BOOL isLogical = (partition.systemID == 0x05 || partition.systemID == 0x0F);
    if (isLogical) {
        printf("Selected partition is part of an extended partition. Checking EBR...\n");

        partitionOffset.QuadPart = (LONGLONG)partition.StartingLBA * SECTOR_SIZE;
        if (!SetFilePointerEx(hDrive, partitionOffset, NULL, FILE_BEGIN)) {
            perror("Failed to set file pointer to EBR");
            CloseHandle(hDrive);
            return 1;
        }

        if (!ReadFile(hDrive, &ebr, sizeof(EBR), &bytesRead, NULL)) {
            perror("Failed to read EBR");
            CloseHandle(hDrive);
            return 1;
        }

        if (ebr.signature != 0xAA55) {
            printf("Invalid EBR signature: 0x%04X\n", ebr.signature);
            CloseHandle(hDrive);
            return 1;
        }

        ebr.partition.bootIndicator = 0x80;
        partition = ebr.partition;
        partitionOffset.QuadPart += SECTOR_SIZE;
    } else {
        mbr.partitions[partitionNum].bootIndicator = 0x80;
        partitionOffset.QuadPart = (LONGLONG)partition.StartingLBA * SECTOR_SIZE;
    }

    LARGE_INTEGER partitionSize;
    partitionSize.QuadPart = (LONGLONG)partition.totalSectors * SECTOR_SIZE;

    HANDLE hOut = CreateFileA( outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,  FILE_ATTRIBUTE_NORMAL, NULL );

    if (hOut == INVALID_HANDLE_VALUE) {
        perror("Failed to open output image file");
        CloseHandle(hDrive);
        return 1;
    }

    DWORD bytesWritten;
    if (!WriteFile(hOut, &mbr, SECTOR_SIZE, &bytesWritten, NULL)) {
        perror("Failed to write MBR to output file");
        CloseHandle(hDrive);
        CloseHandle(hOut);
        return 1;
    }

    BYTE zeroBuffer[SECTOR_SIZE] = {0};
    LONGLONG sectorsToPartition = isLogical ? (partition.StartingLBA - 1) : (partition.StartingLBA - 1);
    while (sectorsToPartition > 0) {
        if (!WriteFile(hOut, zeroBuffer, SECTOR_SIZE, &bytesWritten, NULL)) {
            perror("Failed to write zero sectors to output file");
            CloseHandle(hDrive);
            CloseHandle(hOut);
            return 1;
        }
        sectorsToPartition--;
    }

    if (isLogical) {
        if (!WriteFile(hOut, &ebr, SECTOR_SIZE, &bytesWritten, NULL)) {
            perror("Failed to write EBR to output file");
            CloseHandle(hDrive);
            CloseHandle(hOut);
            return 1;
        }
    }

    BYTE vbr[SECTOR_SIZE];
    if (!SetFilePointerEx(hDrive, partitionOffset, NULL, FILE_BEGIN)) {
        perror("Failed to set file pointer to partition start");
        CloseHandle(hDrive);
        CloseHandle(hOut);
        return 1;
    }

    if (!ReadFile(hDrive, vbr, SECTOR_SIZE, &bytesRead, NULL)) {
        perror("Failed to read VBR");
        CloseHandle(hDrive);
        CloseHandle(hOut);
        return 1;
    }

    printf("VBR first bytes: %02X %02X %02X %02X\n", vbr[0], vbr[1], vbr[2], vbr[3]);
    if (vbr[3] == 'N' && vbr[4] == 'T' && vbr[5] == 'F' && vbr[6] == 'S') {
        printf("Detected NTFS partition.\n");
    } else if (vbr[0] == 0xEB && vbr[2] == 0x90) {
        printf("Detected FAT32 or similar partition (bootable VBR).\n");
    } else {
        printf("Warning: VBR signature not recognized.\n");
    }

    if (!WriteFile(hOut, vbr, SECTOR_SIZE, &bytesWritten, NULL)) {
        perror("Failed to write VBR to output file");
        CloseHandle(hDrive);
        CloseHandle(hOut);
        return 1;
    }

    BYTE buffer[SECTOR_SIZE * 1024];
    DWORD chunkSize = sizeof(buffer);
    LONGLONG bytesToCopy = partitionSize.QuadPart - SECTOR_SIZE;
    while (bytesToCopy > 0) {
        DWORD toRead = (DWORD)(bytesToCopy > chunkSize ? chunkSize : bytesToCopy);
        if (!ReadFile(hDrive, buffer, toRead, &bytesRead, NULL)) {
            perror("Error reading partition data");
            CloseHandle(hDrive);
            CloseHandle(hOut);
            return 1;
        }
        if (bytesRead != toRead) {
            printf("Warning: Read %u bytes, expected %u bytes\n", bytesRead, toRead);
            break;
        }

        if (!WriteFile(hOut, buffer, bytesRead, &bytesWritten, NULL)) {
            perror("Error writing to output file");
            CloseHandle(hDrive);
            CloseHandle(hOut);
            return 1;
        }
        if (bytesWritten != bytesRead) {
            printf("Warning: Wrote %u bytes, expected %u bytes\n", bytesWritten, bytesRead);
            break;
        }

        bytesToCopy -= bytesRead;
    }

    if (bytesToCopy > 0) {
        printf("Warning: Not all data was copied. Remaining: %lld bytes\n", bytesToCopy);
    }

    CloseHandle(hDrive);
    CloseHandle(hOut);

    printf("Disk image created successfully: %s\n", outputPath);
    return 0;
}



int DumpBootToBin(int driveNumber, int partitionNumber, const char* bootFilename) {
    printf("\n--------------DumpBootToBin----------------\n Disk=%d  Part=%d  %s\n", driveNumber, partitionNumber, bootFilename);

    char physicalDrivePath[64];
    sprintf(physicalDrivePath, "\\\\.\\PhysicalDrive%d", driveNumber);

    HANDLE hDrive = CreateFileA( physicalDrivePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL  );

    if (hDrive == INVALID_HANDLE_VALUE) {
        perror("Failed to open physical drive");
        return 1;
    }

    DWORD bytesRead;
    MBR mbr;

    if (!ReadFile(hDrive, &mbr, sizeof(MBR), &bytesRead, NULL)) {
        perror("Failed to read MBR");
        CloseHandle(hDrive);
        return 1;
    }

    if (mbr.signature != 0xAA55) {
        printf("Invalid MBR signature: 0x%04X\n", mbr.signature);
        CloseHandle(hDrive);
        return 1;
    }

    if (partitionNumber < 0 || partitionNumber > 3) {
        printf("Invalid partition number. Must be 0-3\n");
        CloseHandle(hDrive);
        return 1;
    }

    PARTITION_ENTRY partition = mbr.partitions[partitionNumber];

    if (partition.totalSectors == 0) {
        printf("Selected partition is empty or not valid.\n");
        CloseHandle(hDrive);
        return 1;
    }

    LARGE_INTEGER partitionOffset;
    EBR ebr;
    BOOL isLogical = (partition.systemID == 0x05 || partition.systemID == 0x0F);
    if (isLogical) {
        printf("Selected partition is part of an extended partition. Checking EBR...\n");

        partitionOffset.QuadPart = (LONGLONG)partition.StartingLBA * SECTOR_SIZE;
        if (!SetFilePointerEx(hDrive, partitionOffset, NULL, FILE_BEGIN)) {
            perror("Failed to set file pointer to EBR");
            CloseHandle(hDrive);
            return 1;
        }

        if (!ReadFile(hDrive, &ebr, sizeof(EBR), &bytesRead, NULL)) {
            perror("Failed to read EBR");
            CloseHandle(hDrive);
            return 1;
        }

        if (ebr.signature != 0xAA55) {
            printf("Invalid EBR signature: 0x%04X\n", ebr.signature);
            CloseHandle(hDrive);
            return 1;
        }

        partition = ebr.partition;
        partitionOffset.QuadPart += SECTOR_SIZE;
    } else {
        partitionOffset.QuadPart = (LONGLONG)partition.StartingLBA * SECTOR_SIZE;
    }

    BYTE vbr[SECTOR_SIZE];
    if (!SetFilePointerEx(hDrive, partitionOffset, NULL, FILE_BEGIN)) {
        perror("Failed to set file pointer to VBR");
        CloseHandle(hDrive);
        return 1;
    }

    if (!ReadFile(hDrive, vbr, SECTOR_SIZE, &bytesRead, NULL)) {
        perror("Failed to read VBR");
        CloseHandle(hDrive);
        return 1;
    }

    printf("VBR first bytes: %02X %02X %02X %02X\n", vbr[0], vbr[1], vbr[2], vbr[3]);
    if (vbr[3] == 'N' && vbr[4] == 'T' && vbr[5] == 'F' && vbr[6] == 'S') {
        printf("Detected NTFS partition.\n");
    } else if (vbr[0] == 0xEB && vbr[2] == 0x90) {
        printf("Detected FAT32 or similar partition.\n");
    } else {
        printf("Warning: VBR signature not recognized.\n");
    }

    HANDLE hOut = CreateFileA( bootFilename, GENERIC_WRITE, 0, NULL,  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,  NULL  );

    if (hOut == INVALID_HANDLE_VALUE) {
        perror("Failed to open output boot file");
        CloseHandle(hDrive);
        return 1;
    }

    DWORD bytesWritten;
    if (!WriteFile(hOut, vbr, SECTOR_SIZE, &bytesWritten, NULL)) {
        perror("Failed to write VBR to file");
        CloseHandle(hDrive);
        CloseHandle(hOut);
        return 1;
    }

    CloseHandle(hDrive);
    CloseHandle(hOut);
    return 0;
}

int DumpMBRToBin( int driveNumber, const char* mbrFilename) {
    printf("\n--------------DumpMBRToBin----------------\n Disk=%d  %s\n", driveNumber,  mbrFilename);

    char physicalDrivePath[64];
    sprintf(physicalDrivePath, "\\\\.\\PhysicalDrive%d", driveNumber);

    HANDLE hDrive = CreateFileA(
        physicalDrivePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hDrive == INVALID_HANDLE_VALUE) {
        perror("Failed to open physical drive");
        return 1;
    }

    DWORD bytesRead;
    MBR mbr;

    if (!ReadFile(hDrive, &mbr, sizeof(MBR), &bytesRead, NULL)) {
        perror("Failed to read MBR");
        CloseHandle(hDrive);
        return 1;
    }

    if (mbr.signature != 0xAA55) {
        printf("Invalid MBR signature: 0x%04X\n", mbr.signature);
        CloseHandle(hDrive);
        return 1;
    }

    HANDLE hOut = CreateFileA(
        mbrFilename,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hOut == INVALID_HANDLE_VALUE) {
        perror("Failed to open output MBR file");
        CloseHandle(hDrive);
        return 1;
    }

    DWORD bytesWritten;
    if (!WriteFile(hOut, &mbr, SECTOR_SIZE, &bytesWritten, NULL)) {
        perror("Failed to write MBR to file");
        CloseHandle(hDrive);
        CloseHandle(hOut);
        return 1;
    }

    CloseHandle(hDrive);
    CloseHandle(hOut);
    return 0;
}






void wrtImg_Disk(int diskNum, const char* inFile) {
    printf("\n--------------wrtImg_Disk----------------\n Disk=%d  %s\n", diskNum, inFile);

    char diskPath[64];
    sprintf(diskPath, "\\\\.\\PhysicalDrive%d", diskNum);

    HANDLE hDisk = CreateFileA(diskPath, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hDisk == INVALID_HANDLE_VALUE) {
        printf("Failed to open disk %d. Error: %lu\n", diskNum, GetLastError());
        return;
    }

    // Get disk size
    GET_LENGTH_INFORMATION info;
    DWORD bytesReturned;
    if (!DeviceIoControl(hDisk, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &info, sizeof(info), &bytesReturned, NULL)) {
        printf("Failed to get disk size. Error: %lu\n", GetLastError());
        CloseHandle(hDisk);
        return;
    }
    ULONGLONG diskSize = info.Length.QuadPart;

    // Open the input file
    FILE* f = fopen(inFile, "rb");
    if (!f) {
        printf("Failed to open input file %s. Error: %d\n", inFile, errno);
        CloseHandle(hDisk);
        return;
    }

    // Check file size
    fseek(f, 0, SEEK_END);
    ULONGLONG fileSize = _ftelli64(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize > diskSize) {
        printf("Error: Image file (%.2f GB) is larger than disk (%.2f GB)\n",
               fileSize / (1024.0 * 1024 * 1024), diskSize / (1024.0 * 1024 * 1024));
        fclose(f);
        CloseHandle(hDisk);
        return;
    }

    // Allocate a buffer
    BYTE* buffer = (BYTE*)malloc(BUFFER_SIZE);
    if (!buffer) {
        printf("Memory allocation failed\n");
        fclose(f);
        CloseHandle(hDisk);
        return;
    }

    // Reading and writing data
    ULONGLONG totalWritten = 0;
    DWORD bytesRead, bytesWritten;
    while (totalWritten < fileSize) {
        ULONGLONG toRead = (fileSize - totalWritten) > BUFFER_SIZE ? BUFFER_SIZE : (fileSize - totalWritten);

        bytesRead = fread(buffer, 1, (size_t)toRead, f);
        if (bytesRead == 0) {
            printf("Read error at offset %llu. Error: %d\n", totalWritten, errno);
            free(buffer);
            fclose(f);
            CloseHandle(hDisk);
            return;
        }

        if (!WriteFile(hDisk, buffer, bytesRead, &bytesWritten, NULL) || bytesWritten != bytesRead) {
            printf("Write error at offset %llu. Error: %lu\n", totalWritten, GetLastError());
            free(buffer);
            fclose(f);
            CloseHandle(hDisk);
            return;
        }

        totalWritten += bytesWritten;
        printf("\rProgress: %.2f MB", totalWritten / (1024.0 * 1024.0));
        fflush(stdout);
    }

    // Freeing up resources
    free(buffer);
    fclose(f);
    CloseHandle(hDisk);

    printf("\nImage written to disk %d: %s (%.2f GB)\n", diskNum, inFile, fileSize / (1024.0 * 1024 * 1024));
}

//===========================================================================================================================
int wrtImg_Disk_part(int driveNumber, int partitionNumber, const char* inputFilename ) {
    printf("\n--------------wrtImg_Disk_part----------------\n Disk=%d  Part=%d  %s\n", driveNumber, partitionNumber, inputFilename);

    char physicalDrivePath[64];
    sprintf(physicalDrivePath, "\\\\.\\PhysicalDrive%d", driveNumber);

    HANDLE hDrive = CreateFileA( physicalDrivePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,  NULL  );

    if (hDrive == INVALID_HANDLE_VALUE) {
        perror("Failed to open physical drive");
        return 1;
    }

    HANDLE hIn = CreateFileA(inputFilename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL  );

    if (hIn == INVALID_HANDLE_VALUE) {
        perror("Failed to open input image file");
        CloseHandle(hDrive);
        return 1;
    }

    DWORD bytesRead, bytesWritten;
    MBR mbr;

    if (!ReadFile(hIn, &mbr, sizeof(MBR), &bytesRead, NULL)) {
        perror("Failed to read MBR from image");
        CloseHandle(hDrive);
        CloseHandle(hIn);
        return 1;
    }

    if (mbr.signature != 0xAA55) {
        printf("Invalid MBR signature in image: 0x%04X\n", mbr.signature);
        CloseHandle(hDrive);
        CloseHandle(hIn);
        return 1;
    }

    if (partitionNumber < 0 || partitionNumber > 3) {
        printf("Invalid partition number. Must be 0-3\n");
        CloseHandle(hDrive);
        CloseHandle(hIn);
        return 1;
    }

    PARTITION_ENTRY partition = mbr.partitions[partitionNumber];

    if (partition.totalSectors == 0) {
        printf("Selected partition is empty or not valid.\n");
        CloseHandle(hDrive);
        CloseHandle(hIn);
        return 1;
    }

    LARGE_INTEGER partitionOffset;
    EBR ebr;
    BOOL isLogical = (partition.systemID == 0x05 || partition.systemID == 0x0F);
    if (isLogical) {
        printf("Writing to logical partition. Checking EBR...\n");

        if (!ReadFile(hIn, &ebr, sizeof(EBR), &bytesRead, NULL)) {
            perror("Failed to read EBR from image");
            CloseHandle(hDrive);
            CloseHandle(hIn);
            return 1;
        }

        if (ebr.signature != 0xAA55) {
            printf("Invalid EBR signature: 0x%04X\n", ebr.signature);
            CloseHandle(hDrive);
            CloseHandle(hIn);
            return 1;
        }

        partition = ebr.partition;
        partitionOffset.QuadPart = (LONGLONG)(partition.StartingLBA + 1) * SECTOR_SIZE;
    } else {
        partitionOffset.QuadPart = (LONGLONG)partition.StartingLBA * SECTOR_SIZE;
    }

    LARGE_INTEGER partitionSize;
    partitionSize.QuadPart = (LONGLONG)partition.totalSectors * SECTOR_SIZE;

    if (!WriteFile(hDrive, &mbr, SECTOR_SIZE, &bytesWritten, NULL)) {
        perror("Failed to write MBR to disk");
        CloseHandle(hDrive);
        CloseHandle(hIn);
        return 1;
    }

    BYTE zeroBuffer[SECTOR_SIZE] = {0};
    LONGLONG sectorsToPartition = isLogical ? (partition.StartingLBA - 1) : (partition.StartingLBA - 1);
    while (sectorsToPartition > 0) {
        if (!WriteFile(hDrive, zeroBuffer, SECTOR_SIZE, &bytesWritten, NULL)) {
            perror("Failed to write zero sectors to disk");
            CloseHandle(hDrive);
            CloseHandle(hIn);
            return 1;
        }
        sectorsToPartition--;
    }

    if (isLogical) {
        if (!WriteFile(hDrive, &ebr, SECTOR_SIZE, &bytesWritten, NULL)) {
            perror("Failed to write EBR to disk");
            CloseHandle(hDrive);
            CloseHandle(hIn);
            return 1;
        }
    }

    BYTE vbr[SECTOR_SIZE];
    if (!ReadFile(hIn, vbr, SECTOR_SIZE, &bytesRead, NULL)) {
        perror("Failed to read VBR from image");
        CloseHandle(hDrive);
        CloseHandle(hIn);
        return 1;
    }

    printf("VBR first bytes: %02X %02X %02X %02X\n", vbr[0], vbr[1], vbr[2], vbr[3]);
    if (vbr[3] == 'N' && vbr[4] == 'T' && vbr[5] == 'F' && vbr[6] == 'S') {
        printf("Detected NTFS partition.\n");
    } else if (vbr[0] == 0xEB && vbr[2] == 0x90) {
        printf("Detected FAT32 or similar partition.\n");
    } else {
        printf("Warning: VBR signature not recognized.\n");
    }

    if (!SetFilePointerEx(hDrive, partitionOffset, NULL, FILE_BEGIN)) {
        perror("Failed to set file pointer to partition start");
        CloseHandle(hDrive);
        CloseHandle(hIn);
        return 1;
    }

    if (!WriteFile(hDrive, vbr, SECTOR_SIZE, &bytesWritten, NULL)) {
        perror("Failed to write VBR to disk");
        CloseHandle(hDrive);
        CloseHandle(hIn);
        return 1;
    }

    BYTE buffer[SECTOR_SIZE * 1024];
    DWORD chunkSize = sizeof(buffer);
    LONGLONG bytesToCopy = partitionSize.QuadPart - SECTOR_SIZE;
    while (bytesToCopy > 0) {
        DWORD toRead = (DWORD)(bytesToCopy > chunkSize ? chunkSize : bytesToCopy);
        if (!ReadFile(hIn, buffer, toRead, &bytesRead, NULL)) {
            perror("Error reading image data");
            CloseHandle(hDrive);
            CloseHandle(hIn);
            return 1;
        }
        if (bytesRead != toRead) {
            printf("Warning: Read %u bytes, expected %u bytes\n", bytesRead, toRead);
            break;
        }

        if (!WriteFile(hDrive, buffer, bytesRead, &bytesWritten, NULL)) {
            perror("Error writing to disk");
            CloseHandle(hDrive);
            CloseHandle(hIn);
            return 1;
        }
        if (bytesWritten != bytesRead) {
            printf("Warning: Wrote %u bytes, expected %u bytes\n", bytesWritten, bytesRead);
            break;
        }

        bytesToCopy -= bytesRead;
    }

    if (bytesToCopy > 0) {
        printf("Warning: Not all data was copied. Remaining: %lld bytes\n", bytesToCopy);
    }

    CloseHandle(hDrive);
    CloseHandle(hIn);
    return 0;
}



//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
void list_disks() {
    for (int i = 0; i < 32; i++) {
        char diskPath[64];
        sprintf(diskPath, "\\\\.\\PhysicalDrive%d", i);

        HANDLE hDevice = CreateFileA(diskPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice == INVALID_HANDLE_VALUE)
            continue;

        printf("----------------------------------------------------------\n");
        printf("PhysicalDrive #%d:\n", i);

        BYTE buffer[1000] = {0};
        STORAGE_PROPERTY_QUERY query = {0};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        DWORD bytesReturned;
        if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &buffer, sizeof(buffer), &bytesReturned, NULL)) {
            STORAGE_DEVICE_DESCRIPTOR* desc = (STORAGE_DEVICE_DESCRIPTOR*)buffer;
            char vendor[64] = "", product[64] = "", serial[64] = "";
            if (desc->VendorIdOffset) strncpy(vendor, (char*)(buffer + desc->VendorIdOffset), sizeof(vendor) - 1);
            if (desc->ProductIdOffset) strncpy(product, (char*)(buffer + desc->ProductIdOffset), sizeof(product) - 1);
            if (desc->SerialNumberOffset) strncpy(serial, (char*)(buffer + desc->SerialNumberOffset), sizeof(serial) - 1);
            printf("  Model: %s %s [%s]\n", vendor, product, serial);
        }

        BYTE sector[SECTOR_SIZE];
        DWORD bytesRead;
        if (ReadFile(hDevice, sector, SECTOR_SIZE, &bytesRead, NULL) && bytesRead == SECTOR_SIZE) {
            MBR* mbr = (MBR*)sector;
            if (mbr->signature == 0xAA55) {
                printf("  Partition Table Type: MBR\n");
                for (int p = 0; p < 4; p++) {
                    PARTITION_ENTRY* part = &mbr->partitions[p];
                    if (part->totalSectors == 0) continue;
                    ULONGLONG offset_bytes = (ULONGLONG)part->StartingLBA * SECTOR_SIZE;
                    ULONGLONG size_bytes = (ULONGLONG)part->totalSectors * SECTOR_SIZE;
                    ULONGLONG size_mb = size_bytes / (1024 * 1024);
                    const char* fsType = get_fs_type_mbr(part->systemID);
                    printf("    Partition %d: Offset = %llu bytes, Size = %llu MB, Type = %s (0x%02X)\n",
                           p, offset_bytes, size_mb, fsType, part->systemID);
                }
            } else {
                printf("  Partition Table Type: Unknown\n");
            }
        }
        CloseHandle(hDevice);
    }
    printf("----------------------------------------------------------\n");
}


//============================================================================================================================
int main(int argc, char* argv[]) {
  for (int i = 1; i < argc; i++) {
     printf("  %d:  %s  \n", i, argv[i]            )   ;
  }
//return 0;
    if (strcmp(argv[1], "help") == 0) {      //=====================================
        printf("  wddx32 help \n"    );
        printf("  wddx32 list \n"    );
        //          0       1         2   3     4      5         6         7           8       9
        printf("  wddx32 create    --disk 0  --output disk0.img                                               \n"   );
        printf("  wddx32 create    --disk 0  --part   0        --output  part0.img                            \n"   );

        printf("  wddx32 dumpmeta  --disk 0  --type   mbr      --output  mbr0.bin                             \n"   );
        printf("  wddx32 dumpmeta  --disk 0  --type   boot     --part    0         --output   bootsector.bin  \n"   );

        printf("  wddx32 write     --disk 0  --part   0        --input   part0.img                            \n"   );
        return 0;

    }else if (strcmp(argv[1], "list")   == 0) {      //=====================================
        list_disks();
        return 0;

    }else if (strcmp(argv[1], "create") == 0) {      //=====================================
        int diskNum = -1;
        int partNum = -1;
        char *outFile = NULL;

        for(int i = 2; i < argc-1; ++i) {
            if (strcmp(argv[i], "--disk") == 0) {       diskNum = atoi(argv[++i]);      }
            if (strcmp(argv[i], "--part") == 0) {       partNum = atoi(argv[++i]);      }
            if (strcmp(argv[i], "--output") == 0) {     outFile = argv[++i];            }
        }
        //printf("\tCreate %d  %d  %s\n", diskNum, partNum, outFile);
        if (diskNum >=0 && partNum >= 0 && outFile!=NULL) {
            crtPartImage(diskNum, partNum, outFile);
        }else if (diskNum >= 0 && outFile!=NULL) {
            crtFullDiskImage(diskNum, outFile);
        }else{
            printf("error <options> Create %d   %d  %s\n", diskNum, partNum, outFile);
            return 1;
        }
        return 0;

    } else if (strcmp(argv[1], "dumpmeta") == 0) {      //=====================================
        int diskNum = -1;
        int partNum = -1;
        char *type = NULL;
        char *outFile = NULL;

        for(int i = 2; i < argc-1; ++i) {
            if (strcmp(argv[i], "--disk") == 0) {      diskNum = atoi(argv[++i]);       }
            if (strcmp(argv[i], "--type") == 0) {      type = argv[++i];                }
            if (strcmp(argv[i], "--part") == 0) {      partNum = atoi(argv[++i]);       }
            if (strcmp(argv[i], "--output") == 0) {    outFile = argv[++i];             }
        }

        if (strcmp(type, "mbr") == 0 && partNum < 0 && outFile!=NULL) {
            DumpMBRToBin(diskNum, outFile);
        }else if (strcmp(type, "boot") == 0 && partNum >= 0 && outFile!=NULL) {
            DumpBootToBin(diskNum, partNum, outFile);
        }else{
            printf("error <options> Dumpmeta \n");
            return 1;
        }
        return 0;

    }else if (strcmp(argv[1], "write") == 0  ) {      //=====================================
        int diskNum = -1;
        int partNum = -1;
        char *inpFile = NULL;

        for(int i = 2; i < argc-1; ++i) {
            if (strcmp(argv[i], "--disk") == 0) {    diskNum = atoi(argv[++i]);      }
            if (strcmp(argv[i], "--part") == 0) {     partNum = atoi(argv[++i]);      }
            if (strcmp(argv[i], "--input") == 0) {    inpFile = argv[++i];            }
        }

        if (diskNum >=0 &&  partNum >= 0 && inpFile!=NULL) {
            wrtImg_Disk_part(diskNum, partNum, inpFile);
        }else if (diskNum >=0 && inpFile!=NULL) {
            wrtImg_Disk(diskNum, inpFile);
        }else{
            printf("error <options> Write \n");
            return 1;
        }

        return 0;
    }

    return 1;
}

//===end===
