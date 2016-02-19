Clevo Fan Control Indicator for Ubuntu
======================================

This program is an Ubuntu indicator to control the fan of Clevo laptops, using reversed-engineering port information from ECView.

It shows the CPU temperature on the left and the GPU temperature on the right, and a menu for manual control.

![Clevo Indicator Screen](http://i.imgur.com/ucwWxLq.png)



For command-line, use *-h* to display help, or a number representing percentage of fan duty to control the fan (from 40% to 100%).



Build and Install
-----------------

```shell
sudo apt-get install libappindicator3-dev
git clone git@github.com:AqD/clevo-indicator.git
cd clevo-indicator
make install
```
