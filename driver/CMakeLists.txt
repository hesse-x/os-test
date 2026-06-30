# driver/ — 用户态驱动进程

add_user_elf(kbd_driver   SOURCES kbd_driver.cc ../user/lib/usb_kbd.cc ../user/lib/input_driver.cc   LINK_LIBS c)
add_user_elf(terminal     SOURCES terminal.cc     LINK_LIBS c)
