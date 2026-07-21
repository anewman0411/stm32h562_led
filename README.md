## To flash 

### First ENSURE that you have OpenOCD version 0.12.0. 

#### ** NOTE ** Whether or not you want the program flashed, to be running immediately you can modify the openocd.cfg and view the comments in the file to change the flash settings. By default it runs the program on flashing.

### Run OpenOCD Version Check

bash 
```
openocd --version
```


You should see

openocd --version
Open On-Chip Debugger 0.12.0+dev-00635-g0a084c293 (2026-02-12-23:14) [https://github.com/STMicroelectronics/OpenOCD]
Licensed under GNU GPL v2
For bug reports, read
	http://openocd.org/doc/doxygen/bugs.html


## Now to make the project

bash 
```
mkdir build
cd build
cmake ..
make
```


#### And then FLASH it


bash 
```
openocd -f ../openocd.cfg -c "init" -c "halt" -c "flash write_image erase ../bin/main.elf" -c "verify_image ../bin/main.elf" -c "reset run" -c "exit"

```
