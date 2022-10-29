
# This module originally comes from
# https://github.com/henryk/fhem-co2mini/raw/master/FHEM/38_co2mini.pm
# License: GPLv2.
# it has been patched a little.

package main;

use strict;
use warnings;

use POSIX;
use Fcntl;
use Errno;

# Key retrieved from /dev/random, guaranteed to be random ;-)
my $key = "u/R\xf9R\x7fv\xa5";

my $timeout = 300;

sub
co2mini_Initialize($)
{
  my ($hash) = @_;

  $hash->{DefFn}    = "co2mini_Define";
  $hash->{ReadyFn}  = "co2mini_Ready";
  $hash->{ReadFn}   = "co2mini_Read";
  $hash->{UndefFn}  = "co2mini_Undefine";
  $hash->{AttrFn}   = "co2mini_Attr";
  $hash->{AttrList} = "disable:0,1 showraw:0,1 ".
                      $readingFnAttributes;
}

#####################################

sub
co2mini_Define($$)
{
  my ($hash, $def) = @_;

  my @a = split("[ \t][ \t]*", $def);

  if (@a < 2) {
    return "Usage: define <name> co2mini [devicenode or ip:port]";
  }
  
  my $name = $a[0];
  my $dev = $a[2] // "/dev/co2mini0";
  if (($dev !~ m/\@/) && ($dev !~ m/\:/)) {
    $dev .= "\@directio"; # We need to append @directio or DevIo will try to set baudrates and stuff...
  }
  $hash->{NAME} = $name;
  $hash->{DeviceName} = $dev;
  
  return DevIo_OpenDev($hash, 0, "co2mini_OnConnect");
}

sub
co2mini_OnConnect($)
{
  my ($hash) = @_;
  my $name   = $hash->{NAME};
  Log3 $name, 3, "$name: co2mini_OnConnect";

  # Initialize stuff if this is a local device
  if (defined($hash->{DIODev})) {
    # Result of printf("0x%08X\n", HIDIOCSFEATURE(9)); in C
    my $HIDIOCSFEATURE_9 = 0xC0094806;
  
    # Send a FEATURE Set_Report with our key
    unless (ioctl($hash->{DIODev}, $HIDIOCSFEATURE_9, "\x00".$key)) {
      Log3 $name, 3, "$name: IOCTL failed.";
      return "Error establishing connection to CO2mini at " . $hash->{DeviceName};
    }
  }
  readingsSingleUpdate($hash, "state", "opened", 1);
  
  $hash->{LAST_RECV} = time();
  co2mini_Timer($hash);
  
  return undef;
}

sub
co2mini_Ready($)
{
  my ($hash) = @_;
  my $name = $hash->{NAME};

  return undef if( AttrVal($name, "disable", 0 ) == 1 );

  if ($hash->{STATE} eq "disconnected") {
    return DevIo_OpenDev($hash, 1, "co2mini_OnConnect");
  }
}

sub
co2mini_Disconnect($)
{
  my ($hash) = @_;
  my $name   = $hash->{NAME};
  
  if ((ReadingsVal($name, "state", "") eq "opened")
   || (ReadingsVal($name, "state", "") eq "connected")) {
    DevIo_CloseDev($hash);
  }

  readingsSingleUpdate($hash, "state", "disconnected", 1);
}


# Input: string key, string data
# Output: array of integers result
sub
co2mini_decrypt($$)
{
  my @key = map { ord } split //, shift;
  my @data = map { ord } split //, shift;
  my @offset = (0x84,  0x47,  0x56,  0xD6,  0x07,  0x93,  0x93,  0x56);
  my @shuffle = (2, 4, 0, 7, 1, 6, 5, 3);
  
  my @phase1 = map { $data[$_] } @shuffle;
  
  my @phase2 = map { $phase1[$_] ^ $key[$_] } (0 .. 7);
  
  my @phase3 = map { ( ($phase2[$_] >> 3) | ($phase2[ ($_-1+8)%8 ] << 5) ) & 0xff; } (0 .. 7);
  
  my @result = map { (0x100 + $phase3[$_] - $offset[$_]) & 0xff; } (0 .. 7);
  
  return @result;
}


sub
co2mini_UpdateData($$@)
{
  my ($hash, $showraw, @data) = @_;
  my $name = $hash->{NAME};

  Log3 $name, 5, "co2mini data received " . join(" ", @data);
  if($#data < 4) {
    Log3 $name, 3, "co2mini incoming data too short";
    return;
  }
  elsif($data[4] != 0x0d) {
    Log3 $name, 3, "co2mini unexpected byte 5";
    return;
  }
  elsif((($data[0] + $data[1] + $data[2]) & 0xff) != $data[3]) {
    Log3 $name, 3, "co2mini checksum error";
    return;
  }

  my ($item, $val_hi, $val_lo, $rest) = @data;
  my $value = $val_hi << 8 | $val_lo;
    
  if ($item == 0x50) {
    readingsBulkUpdate($hash, "co2", $value);
  } elsif ($item == 0x42) {
    readingsBulkUpdate($hash, "temperature", $value/16.0 - 273.15);
  } elsif ($item == 0x44) {
    readingsBulkUpdate($hash, "humidity", $value/100.0);
  } elsif ($item == 0x41) {
    readingsBulkUpdate($hash, "humidity", $value/100.0);
  }
  if ($showraw) {
    readingsBulkUpdate($hash, sprintf("raw_%02X", $item), $value);
  }
}

sub
co2mini_Read($)
{
  my ($hash) = @_;
  my $name   = $hash->{NAME};
  my ($buf, $readlength);

  my $showraw = AttrVal($name, "showraw", 0);

  readingsBeginUpdate($hash);

  if ($hash->{DeviceName} =~ m/\@directio$/) {
    $buf = DevIo_SimpleRead($hash);

    $readlength = length $buf;
    if (defined($buf) || ($readlength > 0)) {
      my @data = map { ord } split(//, $buf);
      # Some sensors send the data unencrypted, we try to find out if that is
      # the case here.
      if (($data[4] != 0x0d)
       || ((($data[0] + $data[1] + $data[2]) & 0xff) != $data[3])) {
        # Does not seem to be valid without decryption, so try to decrypt it.
        @data = co2mini_decrypt($key, $buf);
      }
      co2mini_UpdateData($hash, $showraw, @data);
    }
  } else {
    $buf = DevIo_SimpleRead($hash);

    $readlength = length $buf;
    if (defined($buf) || ($readlength > 0)) {
      $hash->{helper}{buf} .= $buf;
      while ($hash->{helper}{buf} =~ /^(.{4,}\x0d)/s) {
        my @data = map { ord } split //, $1;
        substr($hash->{helper}{buf}, 0, $#data+1) = '';

        co2mini_UpdateData($hash, $showraw, @data);
      }
    } else {
      Log3 $name, 1, "co2mini network error or disconnected: $!";
    }
  }
  $hash->{LAST_RECV} = time();
  readingsSingleUpdate($hash, "state", "connected", 1);

  if (!defined($readlength)) {
    if ($!{EAGAIN} or $!{EWOULDBLOCK}) {
      # This is expected, ignore it
    } else {
      Log3 $name, 1, "co2mini device error or disconnected: $!";
    }
  } elsif (defined($hash->{DIODev}) && ($readlength != 8)) {
    Log3 $name, 3, "co2mini incomplete data received, shouldn't happen, ignored";
  }

  readingsEndUpdate($hash, 1);
}

sub
co2mini_Undefine($$)
{
  my ($hash, $arg) = @_;

  RemoveInternalTimer($hash);

  co2mini_Disconnect($hash);

  return undef;
}

sub
co2mini_Attr($$$)
{
  my ($cmd, $name, $attrName, $attrVal) = @_;

  if( $attrName eq "disable" ) {
    my $hash = $defs{$name};
    if( $cmd eq "set" && $attrVal ne "0" ) {
      co2mini_Disconnect($hash);
    } else {
      my $dev = $hash->{DeviceName};
      $readyfnlist{"$name.$dev"} = $hash;
    }
  }

  return;
}

sub
co2mini_Timer($)
{
  my ($hash) = @_;
  my $name   = $hash->{NAME};
  # First set ourselves again
  RemoveInternalTimer($hash);
  InternalTimer(time() + ($timeout / 2), "co2mini_Timer", $hash, 0);
  # Now check if we actually need to do something.
  my $lastRecvDiff = (time() - $hash->{LAST_RECV});
  if ($lastRecvDiff > $timeout) {
    Log3 $name, 3, "co2mini_Timer: timeout $timeout seconds has passed. Trying a reconnect.";
    co2mini_Disconnect($hash);
    DevIo_OpenDev($hash, 0, "co2mini_OnConnect");
  }
  return;
}

1;

=pod
=begin html

<a name="co2mini"></a>
<h3>co2mini</h3>
<ul>
  Module for measuring temperature and air CO2 concentration with a co2mini like device. 
  These are available under a variety of different branding, but all register as a USB HID device
  with a vendor and product ID of 04d9:a052.
  For photos and further documentation on the reverse engineering process see
  <a href="https://hackaday.io/project/5301-reverse-engineering-a-low-cost-usb-co-monitor">Reverse-Engineering a low-cost USB CO₂ monitor</a>.<br><br>

  Alternatively you can use a remote sensor with the <tt>co2mini_server.pl</tt> available at <a href="https://github.com/henryk/fhem-co2mini">https://github.com/henryk/fhem-co2mini</a>.
  This script needs to be started with two arguments: the device node of the co2mini device and a port number to listen on. It will then listen on this port and accept connections from clients.
  Clients get a stream of decrypted messages from the CO2 monitor (that is: 5 bytes up to and including the 0x0D each).
  When configuring the FHEM module to connect to a remote <tt>co2mini_server.pl</tt>, simply supply <tt>address:port</tt> instead of the device node.<br><br>

  Notes:
  <ul>
    <li>FHEM, or the user running <tt>co2mini_server.pl</tt>, has to have permissions to open the device. To configure this with udev, put a file named <tt>90-co2mini.rules</tt>
        into <tt>/etc/udev/rules.d</tt> with this content:
<pre>ACTION=="remove", GOTO="co2mini_end"

SUBSYSTEMS=="usb", KERNEL=="hidraw*", ATTRS{idVendor}=="04d9", ATTRS{idProduct}=="a052", GROUP="plugdev", MODE="0660", SYMLINK+="co2mini%n", GOTO="co2mini_end"

LABEL="co2mini_end"
</pre> where <tt>plugdev</tt> would be a group that your process is in.</li>
  </ul><br>

  <a name="co2mini_Define"></a>
  <b>Define</b>
  <ul>
    <code>define &lt;name&gt; co2mini [devicenode or address:port]</code><br>
    <br>

    Defines a co2mini device. Optionally a device node may be specified, otherwise this defaults to <tt>/dev/co2mini0</tt>.<br>
    Instead of a device node, a remote server can be specified by using <tt>address:port</tt>.<br><br>

    Examples:
    <ul>
      <code>define co2 co2mini</code><br>
    </ul>
    Example (network):
    <ul>
      <code>define co2 co2mini raspberry:23231</code><br>
    </ul>
    (also: on the host named <tt>raspberry</tt> start a command like <tt>co2mini_server.pl /dev/co2mini0 23231</tt>)
  </ul><br>

  <a name="co2mini_Readings"></a>
  <b>Readings</b>
  <dl><dt>co2</dt><dd>CO2 measurement from the device, in ppm</dd>
    <dt>temperature</dt><dd>temperature measurement from the device, in °C</dd>
    <dt>humidity</dt><dd>humidity measurement from the device, in % (may not be available on your device)</dd>
  </dl>

  <a name="co2mini_Attr"></a>
  <b>Attributes</b>
  <ul>
    <li>disable<br>
      1 -> disconnect</li>
    <li>showraw<br>
      1 -> show raw data as received from the device in readings of the form raw_XX</li>
  </ul>
</ul>

=end html
=cut
