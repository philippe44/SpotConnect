# SpotConnect: Enable Spotify Connect for UPnP and AirPlay devices 
Use these applications to add Spotify Connect capabilities to UPnP (like Sonos) or AirPlay players. Respectively **spotupnp** for UPnP and **spotraop** for AirPlay

SpotConnect can run on any machine that has access to your local network (Windows, MacOS x86 and arm64, Linux x86, x86_64, arm, aarch64, sparc, mips, powerpc, Solaris and FreeBSD). It does not need to be on your main computer. (For example, a Raspberry Pi works well). It will detect UPnP/Sonos or AirPlay players, create as many virtual Spotify Connect devices as needed, and act as a bridge/proxy between Spotify controller (iPhone, iPad, PC, Mac ...) and the real UPnP/Sonos or AirPlay players.

For UPnP, the audio, after being decoded from vorbis, can be sent in plain, or re-encoded using mp3 or flac. The tracks can be sent one-by one and use the capability of UPnP players to do gapless playback by sending the next track ahead of the current one, but not all players support that or might simply be faulty. There is also a 'flow' mode where all tracks are sent in a continuous stream, similar to a webradio. Note that this mode can be brittle with regard to track position. In 'flow' mode, metadata are likely not to be sent, unlesss player supports 'icy' protocol.

For AirPlay, the audio can be re-encoded using ALAC or left as raw PCM. Note that bridging also works with AppleTV, but you need to create a pairing key. This is done by launching the application with the `-l` option and following instructions. A config file with the required `<raop_credentials>` tag is automatically written to the directory from which the application was launched and will be required for further use. For software-based AirPlay emulators like most cheap knock-off, encryption is required (see below) 

**Please read carefully the (credentials)[credentials] paragraph to understand how to handle SPotify credentials**

## Installing

1. Pre-built binaries are in the **SpotConnect-X.Y.Z.zip** file (both spotupnp or spotraop), use the version that matches your OS. You can also look at releases

   * For AirPlay, the file is `spotraop-<os>-<platform>` (so `spotraop-linux-aarch64` for AirPlay on Linux + arm64 CPU)
   * For UPnP/Sonos, the file is `spotupnp-<os>-<platform>` (so `spotupnp-macos-arm64` for UPnP/Sonos on macOS + arm CPU)

1. Store the \<executable\> (e.g. `spotupnp-linux-aarch64multi`) in any directory. 

1. OS-specific steps:
	
   * **macOS**: Install openSSL and do the following steps to use the dynamic load library version:
	   - install openssl: `brew install openssl`. This creates libraries (or at least links) into `/usr/local/opt/openssl[/x.y.z]/lib` where optional 'x.y.z' is a version number
	   - create links to these libraries: 
	   ```
	   ln -s /usr/local/opt/openssl[/x.y.z]/lib/libcrypto.dylib /usr/local/lib/libcrypto.dylib 
	   ln -s /usr/local/opt/openssl[/x.y.z]/lib/libssl.dylib /usr/local/lib/libssl.dylib 
	   ```
    
   * **Non-Windows machines (including macOS)**, open a terminal and change directories to where the executable is stored and run `chmod +x <executable>`. (Example: `chmod +x spotupnp-osx-multi`). Note that if you choose to download the whole repository (instead of individual files) from you web browser and then unzip it, then in the bin/ sub-directory, file permissions should be already set.
     
   * **Windows**: Copy all the .dll as well if you want to use the non-static version or use the [Windows MSVC package](https://learn.microsoft.com/en-US/cpp/windows/latest-supported-vc-redist?view=msvc-170)

1. Don't use firewall or set ports using options below and open them. 
	- Each device uses 1 port for HTTP (use `-a` parameter, default is random)
	- UPnP adds one extra port for discovery (use `-b` or \<upnp_socket\> parameter, default is 49152 and user value must be *above* this)

1. In Docker, you must use 'host' mode to enable audio webserver. Note that you can't have a NAT between your devices and the machine where AirConnect runs.

## Running

Double click the \<executable\> or launch it by typing `./<executable>` in the same command line window. 

You should start to see lots of log messages on screen. Using your iOS/Mac/iTunes/Airfoil/other client, you should now see new AirPlay devices and can try to play audio to them. 

If it works, type `exit`, which terminates the executable, and then, on non-Windows/MacOS machines, relaunch it with `-z` so that it can run in the background and you can close the command line window. You can also start it automatically using any startup script or a Linux service as explained below. Nothing else should be required, no library or anything to install.

For each platform, there is a normal and a '-static' version. This one includes all libraries directly inside the application, so normally there is no dependence to 3rd party shared libraries, including SSL. You can try it if the normal fails to load (especially on old systems), but static linkage is a blessing a curse (exact reasons out of scope of this README). Now, if the static version still does not work, there are other solutons that are pretty technical, see here. Best is that you open an issue if you want help with tha

## Common information:

**Use `-h` for command line details**
- When started in interactive mode (w/o -Z or -z option) a few commands can be typed at the prompt
	- `exit`
	- `save <file>` : save the current configuration in file named [name]
- Volume changes made in native control applications are synchronized with Spotify controller
- Pause made using native control application is sent back to Spotify
- Re-scan for new / lost players happens every 30s
- A config file (default `config.xml`) can be created for advanced tweaking (a reference version can be generated using  the `-i <file>` command line). To specify a config file, use `-x <file>`.
- When you have more than one ethernet card, you case use `-b [ip|iface]` to set what card to bind to. Note that 0.0.0.0 is not authorized
- Use `-l` for AppleTV pairing mode
- Use `-b [ip|iface][:port]` to set network interface (ip@ or interface name as reported by ifconfig/ipconfig) to use and, for spotupnp only, UPnP port to listen to (must be above the default 49152)
- Use `-r` to set Spotify's Vorbis encoding rate
- Use `-a <port>[:<count>]`to specify a port range (default count is 128)
- Use of `-z` disables interactive mode (no TTY) **and** self-daemonizes (use `-p <file>` to get the PID). Use of `-Z` only disables interactive mode 
- <strong>Do not daemonize (using & or any other method) the executable w/o disabling interactive mode (`-Z`), otherwise it will consume all CPU. On Linux, FreeBSD and Solaris, best is to use `-z`. Note that -z option is not available on MacOS or Windows</strong>

## Config file parameters 

The default configuration file is `config.xml`, stored in the same directory as the \<executable\>. Each of "Common" parameters below can be set in the `<common>` section to apply to all devices. It can also be set in any `<device>` section to apply only to a specific device and overload the value set in `<common>`. Use the `-x <config>` command line option to use a config file of your choice.

### Common
- `enabled <0|1>` : in common section, enables new discovered players by default. In a dedicated section, enables the player
- `name`        : The name that will appear for the device in AirPlay. You can change the default name.
- `vorbis_rate <96|160|320>` : set the Spotify bitrate
- `remove_timeout <-1|n>` : set to `-1` to avoid removing devices prematurely

##### UPnP
- `upnp_max`    : set the maximum UPnP version use to search players (default 1)
- `artwork`	: an URL to a fixed artwork to be displayed on player in flow mode
- `flow`        : enable flow mode
- `gapless`     : use UPnP gapless mode (if players supports it)
- `http_content_length`	   : same as `-g` command line parameter
- `codec mp3[:<bitrate>] | flc[:0..9] | wav | pcm`: format used to send HTTP audio. FLAC is recommended but uses more CPU (pcm only available for UPnP). For example, `mp3:320` for 320Kb/s MP3 encoding.

#### AirPlay
- `alac_encode <0|1>`: format used to send audio (`0` = PCM, `1` = ALAC)
- `encryption <0|1>`: most software-based player and cheap knock-off require encryption to be activated otherwise they won't stream.

#### Apple TV
- `raop_credentials`   : Apple TV pairing credential obtained from running in `-l` pairing mode

### Global
These are set in the main `<spotraop>` section:
- `log_limit <-1|n>` 	   : (default -1) when using log file (`-f` parameter), limits its size to 'n' MB (-1 = no limit)
- `max_players`            : set the maximum of players (default 32)
- `ports <port>[:<count>]` : set port range to use (see -a)
- `interface ?|<iface>|<ip>` : set the network interface, ip or autodetect
- `credentials 0|1`        : see below
- `credentials_path <path>`: see below

There are many other parameters, to list all of them, use `-i <config>` to create a default config file.

### Credentials
A player can be discovered using the *ZeroConf* protocol or can spontaneuously register to Spotify servers. When using ZeroConf, the player is by default not registered to Spotify servers and if you use a WebAPI application (e.g. an HomeAutomation service), it will not list it. Here is the reason why:

In ZeroConf, the player simply broadcasts (using mDNS) information to your local network so that a **local** Spotify listener can discover it, query Spotify servers to get credentials and pass these back to the player which then will register to the servers. This is why you don't need to enter any credentials for the player. It's convenient because only the listener stores the sensitive credentials but again, this means that the player will not be "standby" on Spotify servers. 

There is also a way for players to store credentials and immediately register to Spotify servers upon startup. You can either pass your username (-U) and password (-P) on the command line so that SpotConnect has the full information to register, but it's not ideal in term of security and you have to make sure the script that launches it is not readable by unauthorized users. Luckily, Spotify provides a mechansim by which their servers provide device-specific reusable credentials in a form of a token which is less sensitive. SpotConnect can query and store these, using two different methods:

1- Store credentials in the config file: using the `-j` command line option, SpotConnect reads the .xml config file for `<credentials>` JSON tag and uses it to register. When SpotConnect receives such reusable credentials from Spotify, it will also store them into the config file, of course assuming that the `-I` option is set to authorize its update. Note that the '-j' on comamnd line is equivalent to the **root** tag `<credentials>` of the config file. It's a global enablement parameter and cannot be set per player.

2- Store credentials in separated files: using the `-J <path>` command line option, SpotConnect writes and read such reusable credentials in files named `spotraop-XXXXXXXX.json` (respectively spotupnp...). The root tag `<credentials_path>` in config file is equivalent to the `-J` command line to set base path for these files. This can be a slightly better option to secure such credentials (which again are less sensitive, they won't allow access to your account).

SpotConnect can only write reusable credentials when it obtains it. This can done either by running it **once** with `-U` and `-P` and wait for all UPnP/AirPlay players to be discovered, or by playing something onto each player at least once using (e.g.) Spotify desktop application. Of course, at least one of `-J` or `-j' and -I` options must be used to allow storage of these credentials. 

So for example, if you set `-J`, and a credentials files if found, it will be used to register directly onto servers. If there is no file, this SpotConnect player will remain in ZeroConf mode until it is discovered by Spotify desktop application at which time credentials are received and stored and so next time it will register right away.

I don't know for how long these reusable credentials are valid. In case they become unusable, you can just use -U and -P on command line, it will *force* re-acquisition, or you can you can delete the credential files and erase the `<credentials>` tags in the config file. Note that if you use `-J` and `-j` at the same time, per-player files and config file will be updated but the credentials found in dedicated files have precedence.

## Start automatically in Linux

1. Create a file in `/etc/systemd/system`, e.g. `airupnp.service` with the following content (assuming the airupnp binary is in `/var/lib/airconnect`)

```
[Unit]  
Description=Spotify bridge  
After=network-online.target  
Wants=network-online.target  

[Service]  
ExecStart=/var/lib/spotconnect/spotupnp-linux-arm -Z -x /var/lib/airconnect/spotupnp.xml   
Restart=on-failure  
RestartSec=30  

[Install]  
WantedBy=multi-user.target   
```
2. Enable the service `sudo systemctl enable spotupnp.service`

3. Start the service `sudo service spotupnp start`

To start or stop manually the service, type `sudo service spotupnp start|stop` in a command line window

To disable the service, type `sudo systemctl disable spotupnp.service`

To view the log, `journalctl -u spotupnp.service`

On rPi lite, add the following to the /boot/cmdline.txt: init=/bin/systemd

## Start automatically in macOS (credits @aiwipro)

Create the file com.spotupnp.bridge.plist in ~/Library/LaunchAgents/ 

```
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.spotupnp.bridge</string>
    <key>ProgramArguments</key>
    <array>
        <string>/[path]/spotupnp-macos</string>
	<string>-Z</string>
        <string>-x</string>
        <string>/[path]/spotupnp.xml</string>
        <string>-f</string>
        <string>/[path]/spotupnp.log</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>LaunchOnlyOnce</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
</dict>
</plist>
```

Where `[path]` is the path where you've stored the spotupnpexecutable (without the []). It can be for example `Users/xxx/spotconnect` where `xxx` is your user name 

## Start automatically under Windows

There are many tools that allow an application to be run as a service. You can try this [one](http://nssm.cc/)

## Synology installation

N/A

## Player specific hints and tips

#### Sonos
The upnp version is often used with Sonos players. When a Sonos group is created, only the master of that group will appear as a Spotify Connect player and others will be removed if they were already detected. If the group is later split, then individual players will re-appear. 

When changing volume of a group, each player's volume is changed trying to respect the relative values. It's not perfect and stil under test now. To reset all volumes to the same value, simply move the cursor to 0 and then to the new value. All players will have the same volume then. You need to use the Sonos application to change individual volumes.

To identify your Sonos players, pick an identified IP address, and visit the Sonos status page in your browser, like `http://192.168.1.126:1400/support/review`. Click `Zone Players` and you will see the identifiers for your players in the `UUID` column.

#### Bose SoundTouch
[@chpusch](https://github.com/chpusch) has found that Bose SoundTouch work well including synchonisation (as for Sonos, you need to use Bose's native application for grouping / ungrouping). I don't have a SoundTouch system so I cannot do the level of slave/master detection I did for Sonos

#### Pioneer/Phorus/Play-Fi
Some of these speakers only support mp3 and require a modified `ProtocolInfo` to stream correctly. This can be done by editing the config file and changing `<codec>flc</codec>` to `<codec>mp3</codec>` and replacing the `<mp3>..</mp3>` line with: 
```
<mp3>http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;DLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=0d500000000000000000000000000000</mp3>
```
Note: you can use the `-i config.xml` to generate a config file if you do not have one.

## Misc tips
 
- When players disappear regularly, it might be that your router is filtering out multicast packets. For example, for a Asus AC-RT68U, you have to login by ssh and run echo 0 > /sys/class/net/br0/bridge/multicast_snooping but it does not stay after a reboot.

- Lots of users seems to have problem with Unify and broadcasting / finding players. Here is a guide https://www.neilgrogan.com/ubnt-sonos/ made by somebody who fixes the issue for his Sonos

- Some older Avahi distributions grab the port mDNS port 5353 for exclusive use, preventing SpotConnect to respond to queries. Please set `disallow-other-stacks=no`in `/etc/avahi/avahi-daemon.conf`

## HTTP & UPnP specificities
### HTTP content-length and transfer modes
Lots of UPnP player have very poor quality HTTP and UPnP stacks, in addition of UPnP itself being a poorly defined/certified standard. One of the main difficulty comes from the fact that AirConnect cannot provide the length of the file being streamed as Spotify does not provide it.

The HTTP standard is clear that the "content-length" header is optional and can be omitted when server does not know the size of the source. If the client is HTTP 1.1 there is another possibility which is to use "chunked" mode where the body of the message is divided into chunks of variable length. This is *explicitely* made for case of unknown source length and an HTTP client that claims to support 1.1 **must** support chunked-encoding.

The default mode of SpotUPnP is "no content-length" (\<http_length\> = -1) but unfortunately, some players can't deal with that. You can then try "chunked-encoding" (\<http_length> = -3) but some players who claim to be HTTP 1.1 do not support it. There is a last resort option to add a large fake `content-length` (\<http_length\> = 0). It is set to 2^31-1, so around 5 hours of playback with flac re-encoding. Note that if player is HTTP 1.0 and http_header is set to -3, SpotUPnP will fallback no content-length. The command line option `-g` has the same effect that \<http_length\> in the \<common\> section of a config file.

This might still not work as some players do not understand that the source is not a randomly accessible (searchable) file and want to get the first(e.g.) 128kB to try to do some smart guess on the length, close the connection, re-open it from the beginning and expect to have the same content. I'm trying to keep a buffer of last recently sent bytes to be able to resend-it, but that does not always works. Normally, players should understand that when they ask for a range and the response is 200 (full content), it *means* the source does not support range request but some don't (I've tried to add a header "accept: no-range but that makes things worse most of the time).

### UPnP/DLNA ProtocolInfo
When sending DLNA/UPnP content, there is a special parameter named `ProtocolInfo` that is found in the UPnP command (DIDL-lite header) and can be also explicitly requested by the player during a GET. That field is automatically built but is subject to a lot of intepretations, so it might be helpful to manually define it and you can do that for pcm, wav, flac and mp3 format using the field in the section \<protocol_info\> in your config file.

There is another part of this filed named DLNA which changes when using the 'flow' mode (sending a single stream) vs sending track by track. The default should work, but UPnP is such a mess that some players do require special versions. You can tweaks these here, search DLNA protocol info for more information.

The description of DIDL-lite, ProtocolInfo and DLNA is way beyond the scope of this README, so you should seek for information before tweaking these.

## Compiling from source
It's a CMake-oriented build, and there is a bash script (built.sh) and Windows one (build.cmd). The bash script is intended for cross-platform build and you might be able to call directly your native compiler, but have a look at the command line in the build.sh to make sure it can work. 

Please see [here](https://github.com/philippe44/cross-compiling/blob/master/README.md#organizing-submodules--packages) to know how to rebuild my apps in general 

# Credits
- Special credit to cspot: https://github.com/feelfreelinux/cspot
- pupnp: https://github.com/pupnp/pupnp
- mbedtls: https://github.com/Mbed-TLS/mbedtls/
