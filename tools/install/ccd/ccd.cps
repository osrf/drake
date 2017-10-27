{
  "Cps-Version": "0.8.0",
  "Name": "ccd",
  "Description": "Library for collision detection between two convex shapes",
  "License": "BSD-3-Clause",
  "Version": "2.0",
  "Default-Components": [":ccd"],
  "Components": {
    "ccd": {
      "Type": "dylib",
      "Location": "/usr/lib/x86_64-linux-gnu/libccd.so",
      "Includes": ["/usr/include"]
    }
  }
}
