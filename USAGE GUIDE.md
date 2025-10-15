# INSTALLATION
  To run the code, we first need to install the following on your laptop/PC:
- C-compilor(VS CODE or any other works)
- pkg-config
- GTK +3
- Glib/GIO
- JSON-GLib
- libcurl

  Please note that the code we have written works properly only on linux, and its best if you use vscode for the running
  Run the following command on terminal to install the required librarys:
  sudo apt update
  sudo apt install build-essential pkg-config libgtk-3-dev libjson-glib-dev libcurl4-gnutls-dev

# COMPILATION
- Firstly anyone who wants to run the code must connect to a common wifi. Then go to the megaeditor code and go to
  line 57 of it.
  Change the I.P. address to the I.P. address of your wifi.
- You might need to add the following as well:
  - In the folder of your code add a .vscode(for me) folder
  - Inside that add the json file added in the repository
- Add the mainserver and megaeditor file to two different files and name them mainserver.c and megaeditor.c respectively.
- Copy the given codes there
- In the terminal execute the following compilation code for mainserver:  
  gcc -o mainserver mainserver.c $(pkg-config --cflags --libs gio-2.0)
- Execute the file using the code:  
  ./mainserver
  You should be able to see something like  
  [YYYY-MM-DD HH:MM:SS] === Collaborative Editor Server ===  
  ...  
  [YYYY-MM-DD HH:MM:SS] Press Ctrl+C to stop the server       
- The server is running successfully!!
- Now create a new terminal. In the new terminal execute the following code for compilation for megaeditor:  
  gcc -o megaeditor megaeditor.c     `pkg-config --cflags --libs gtk+-3.0 gio-2.0 glib-2.0 json-glib-1.0`     `curl-      config --cflags --libs`     -lm -pthread
- Execute the file using the code:  
  ./megaeditor
- You should be able to see a popped up white screen.

  If you are stuck on any of the following steps feel free to debug it!!


# HOW TO USE


