# SimpleDRClient
##Simple Test Command

  ```
  drrun.exe -c SimpleDRClientTest.dll -only_from_app -- notepad.exe
  ```

##What do you need to build this ?

  1. A working Dynamorio Build
  
   [Dynamorio Downloads](https://github.com/DynamoRIO/dynamorio/wiki/Downloads)
       
   Choose a build version from above location or you can try your own build and change the step 2 paths accordingly
  
  2. Set the path in the solution
  
    **Solution -> SimpleDRClientTest (project) -> Project Properties (ALT+Enter) ->
    Configuration Properties -> C/C++ -> General -> Additional Include Directories**
      
    Current Paths are stated below, change these according to your enviornment. These folders are found
    after extracting [DynamoRIO-Windows-6.1.1-3.zip](https://github.com/DynamoRIO/dynamorio/releases/download/release_6_1_1/DynamoRIO-Windows-6.1.1-3.zip) 6.1.1 Release in **F:\FYP2\** folder in my pc

        F:\FYP2\DynamoRIO-Windows-6.1.1-3\DynamoRIO-Windows-6.1.1-3\ext\include
        F:\FYP2\DynamoRIO-Windows-6.1.1-3\DynamoRIO-Windows-6.1.1-3\include
        
  3. Set the linker paths
  
    **Solution -> SimpleDRClientTest (project) -> Project Properties (ALT+Enter) ->
    Configuration Properties -> Linker -> Input -> Additional Dependencies**

        F:\FYP2\DynamoRIO-Windows-6.1.1-3\DynamoRIO-Windows-6.1.1-3\ext\lib32\debug\drmgr.lib
        F:\FYP2\DynamoRIO-Windows-6.1.1-3\DynamoRIO-Windows-6.1.1-3\lib32\debug\dynamorio.lib
