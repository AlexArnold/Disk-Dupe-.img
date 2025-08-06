  wddx32 help 
  wddx32 list 
  wddx32 create    --disk 0  --output disk0.img                                               
  wddx32 create    --disk 0  --part   0        --output  part0.img                            
  wddx32 dumpmeta  --disk 0  --type   mbr      --output  mbr0.bin                             
  wddx32 dumpmeta  --disk 0  --part   0    --type   boot     --output   bootsector.bin  
  wddx32 write     --disk 0  --part   0        --input   part0.img                            


X:\VirtualBox.x64\VBoxManage.exe  convertfromraw    filename.img      filename.vhd    --format VHD
