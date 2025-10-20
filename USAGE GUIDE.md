# INSTALLATION
  To run the code, we first need to install the following on your laptop/PC:
- C-compilor(VS CODE or any other works)
- pkg-config
- GTK +3
- Glib/GIO
- JSON-GLib
- libcurl

  **Please note that the code we have written works properly only on linux, and its best if you use vscode**  
  Run the following command on the main terminal of your computer to install the required librarys:  
  sudo apt update  
  sudo apt install build-essential pkg-config libgtk-3-dev libjson-glib-dev libcurl4-gnutls-dev

# COMPILATION
- Firstly anyone who wants to run the code must connect to a common wifi. Then go to the megaeditor code and go to
  line 61 of it.
  Change the I.P. address to the I.P. address of your wifi.
- You might need to add the following as well:
  - In the folder of your code add a .vscode(for people using VS code) folder
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

  **If you are stuck on any of the following steps feel free to debug it or you can contact me or any other team member as well!!**


# HOW TO USE
- As soon as you run the megaeditor, you will be able to see in the white pop up asking for username and password
- You can create your own username and password, register first and then login
- You will be able to see the user interface of ours.
  The left top part is where you write your text/code
  The left bottom part is the terminal
-  You will have a taskbar on the top where you will be able to see all the rich text features like:
  - Font Size
  - Bold, Underline, Italics
  - Left centered, Central and Right centered (works when you select text)
  - Text Colour
  - Backgorund Colour
  - Clear Format (This feature is not gien in word as well!) (works whn you select text)
- The taskbar also contains features such as:
  - Open: A preexisting file on the device
  - Save: To save version history
  - Save As: To save the text locally on your computer
  - Run: To run an executable code (The terminal will also show the errors in your code if any!!)
  - Insert Media: To insert photo or video on the text
  - History: To see all the version historys and revert to anyone if required
  - AI chat (Sorry, we were unable to make the AI chat feature work by the time we had our submission)
  - USER chat: To chat with other users on the same IP
 - The File option is where you can Sign Out or Close the editor
  
    
 
    


