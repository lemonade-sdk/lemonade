Notes on how we can update the different packages for ixwebsocket.

## VCPKG

Visit the [releases](https://github.com/machinezone/IXWebSocket/releases) page on Github. A tag must have been made first.

Download the latest entry.

```
$ cd /tmp
/tmp$ curl -s -O -L https://github.com/machinezone/IXWebSocket/archive/v9.1.9.tar.gz
/tmp$
/tmp$ openssl sha512 v9.1.9.tar.gz
SHA512(v9.1.9.tar.gz)= f1fd731b5f6a9ce6d6d10bee22a5d9d9baaa8ea0564d6c4cd7eb91dcb88a45c49b2c7fdb75f8640a3589c1b30cee33ef5df8dcbb55920d013394d1e33ddd3c8e
```

Now go punch those values in the vcpkg ixwebsocket port config files. Here is what the diff look like.

```
vcpkg$ git diff
diff --git a/ports/ixwebsocket/CONTROL b/ports/ixwebsocket/CONTROL
index db9c2adc9..4acae5c3f 100644
--- a/ports/ixwebsocket/CONTROL
+++ b/ports/ixwebsocket/CONTROL
@@ -1,5 +1,5 @@
 Source: ixwebsocket
-Version: 8.0.5
+Version: 9.1.9
 Build-Depends: zlib
 Homepage: https://github.com/machinezone/IXWebSocket
 Description: Lightweight WebSocket Client and Server + HTTP Client and Server
diff --git a/ports/ixwebsocket/portfile.cmake b/ports/ixwebsocket/portfile.cmake
index de082aece..68e523a05 100644
--- a/ports/ixwebsocket/portfile.cmake
+++ b/ports/ixwebsocket/portfile.cmake
@@ -1,8 +1,8 @@
 vcpkg_from_github(
     OUT_SOURCE_PATH SOURCE_PATH
     REPO machinezone/IXWebSocket
-    REF v8.0.5
-    SHA512 9dcc20d9a0629b92c62a68a8bd7c8206f18dbd9e93289b0b687ec13c478ce9ad1f3563b38c399c8277b0d3812cc78ca725786ba1dedbc3445b9bdb9b689e8add
+    REF v9.1.9
+    SHA512 f1fd731b5f6a9ce6d6d10bee22a5d9d9baaa8ea0564d6c4cd7eb91dcb88a45c49b2c7fdb75f8640a3589c1b30cee33ef5df8dcbb55920d013394d1e33ddd3c8e
 )
```

You will need a fork of the vcpkg repo to make a pull request.

```
git fetch upstream
git co master
git reset --hard upstream/master
git push origin master --force
```

Make the pull request (I use a new branch to do that).

```
vcpkg$ git co -b feature/ixwebsocket_9.1.9
M	ports/ixwebsocket/CONTROL
M	ports/ixwebsocket/portfile.cmake
Switched to a new branch 'feature/ixwebsocket_9.1.9'
vcpkg$
vcpkg$
vcpkg$ git commit -am 'ixwebsocket: update to 9.1.9'
[feature/ixwebsocket_9.1.9 8587a4881] ixwebsocket: update to 9.1.9
 2 files changed, 3 insertions(+), 3 deletions(-)
vcpkg$
vcpkg$ git push
fatal: The current branch feature/ixwebsocket_9.1.9 has no upstream branch.
To push the current branch and set the remote as upstream, use

    git push --set-upstream origin feature/ixwebsocket_9.1.9

vcpkg$ git push --set-upstream origin feature/ixwebsocket_9.1.9

Enumerating objects: 11, done.
Counting objects: 100% (11/11), done.
Delta compression using up to 8 threads
Compressing objects: 100% (6/6), done.
Writing objects: 100% (6/6), 621 bytes | 621.00 KiB/s, done.
Total 6 (delta 4), reused 0 (delta 0)
remote: Resolving deltas: 100% (4/4), completed with 4 local objects.
remote:
remote: Create a pull request for 'feature/ixwebsocket_9.1.9' on GitHub by visiting:
remote:      https://github.com/bsergean/vcpkg/pull/new/feature/ixwebsocket_9.1.9
remote:
To https://github.com/bsergean/vcpkg.git
 * [new branch]          feature/ixwebsocket_9.1.9 -> feature/ixwebsocket_9.1.9
Branch 'feature/ixwebsocket_9.1.9' set up to track remote branch 'feature/ixwebsocket_9.1.9' from 'origin' by rebasing.
vcpkg$
```

Just visit this url, https://github.com/bsergean/vcpkg/pull/new/feature/ixwebsocket_9.1.9, printed on the console, to make the pull request.
