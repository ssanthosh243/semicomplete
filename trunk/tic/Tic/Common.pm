# $Id$
package Tic::Common;

use strict;
use vars ('@ISA', '@EXPORT');
use Exporter;
use Term::ReadKey;
use POSIX qw(strftime);

@ISA = qw(Exporter);
@EXPORT = qw(out real_out error debug query deep_copy prettyprint prettylog);

my $state;

sub set_state { 
	my $self = shift;
	#debug("Setting state for ::Common");
	$state = shift;
}

sub import {
	#debug("Importing from Tic::Common");
	Tic::Common->export_to_level(1,@_);
}

sub out { 
	real_out("\r\e[2K",join("\n",@_) . "\n");
	fix_inputline();
}

sub real_out { print @_; }

sub error {
	print STDERR "\r\e[2K";
  	print STDERR "*> " . shift() . "\n"; 
	fix_inputline();
}
sub debug { foreach (@_) { print STDERR "debug> $_\n"; } }

sub fix_inputline {
	# Fix the line
	if (defined($state->{"input_line"})) {
		my $back = (length($state->{"input_line"}) - $state->{"input_position"});
		real_out($state->{"input_line"});
		real_out("\e[" . $back . "D") if ($back > 0);
	}
}

sub prettyprint {
	my ($state, $type, $data) = @_;
	my $output;

	if ($type eq "help") {
		out();
		real_out($data->{"help"} . "\n");
		return;
	}

	if (($state->{"timestamp"}) || (select() ne "main::STDOUT")) {
		my $timestamp;
		if (select() eq 'main::STDOUT') {
			$timestamp = $state->{"config"}->{"timestamp"};
		} else {
			$timestamp = $state->{"config"}->{"logstamp"};
		}

		$output = strftime($timestamp, localtime(time()));
		$output .= " ";
	}

	$output .= $state->{"config"}->{"$type"};

	# What are the escapes?
	# %% - literal %
	# %S - your own screen name
	# %a - alias name
	# %c - chatroom related to this message
	# %e - error message
	# %g - Group user belongs to
	# %h - help topic
	# %i - idle time of user 
	# %m - Message being sent
	# %s - Screen name of target (or who is messaging you)
	# %v - alias value
	# %w - warning level

	$data->{"sn"} = getrealsn($state, $data->{"sn"});

	if (ref($data) eq "HASH") {
		$output =~ s/%s/$data->{"sn"}/g if (defined($data->{"sn"}));
		$output =~ s/%m/$data->{"msg"}/g if (defined($data->{"msg"}));
		$output =~ s/%g/$data->{"group"}/g if (defined($data->{"group"}));
		$output =~ s/%c/$data->{"chat"}/g if (defined($data->{"chat"}));
		$output =~ s/%w/$data->{"warn"}/g if (defined($data->{"warn"}));
		$output =~ s/%i/$data->{"idle"}/g if (defined($data->{"idle"}));
		$output =~ s/%e/$data->{"error"}/g if (defined($data->{"error"}));
		$output =~ s/%h/$data->{"subject"}/g if (defined($data->{"subject"}));
		$output =~ s/%a/$data->{"alias"}/g if (defined($data->{"alias"}));
		$output =~ s/%v/$data->{"value"}/g if (defined($data->{"value"}));
		$output =~ s/%S/$state->{"sn"}/g;
		$output =~ s/%%/%/g;
	}

	if ($type =~ m/^error/) {
		error($output);
	} else {
		out("< $type > $output");
	}
}

sub prettylog {
	my ($state, $type, $data) = @_;
	if (defined($data)) {
		if (defined($data->{"sn"})) {
			my $sn = getrealsn($state,$data->{"sn"});
			open(IMLOG, ">> ".$ENV{HOME}."/.tic/" . $sn . ".log") or 
				die("Failed trying to open ~/.tic/".$sn." - $!\n");
			select IMLOG;
			prettyprint(@_);
			select STDOUT;
			close(IMLOG);
		}
	}
}

sub query {
   my ($q, $hide) = @_;
   real_out("$q");
   #stty("-echo") if ($hide);
	ReadMode(0) unless ($hide);
   chomp($q = <STDIN>);
   #stty("echo") if ($hide);
   ReadMode(3);
   out() if ($hide);
   return $q;
}

sub stty {
	#return $state->{"stty"} if (!defined($_[0]));
	#$state->{"stty"} = IO::Stty::stty(\*STDIN, "-g");
	return IO::Stty::stty(@_);
}

sub deep_copy {
	my ($data) = shift;
	my $foo;

	if (ref($data) eq "HASH") {
		foreach my $key (keys(%{$data})) {
			$foo->{$key} = $data->{$key};
		}
	}
	return $foo;
}

sub getrealsn {
	my ($state,$sn) = @_;
	my $foo = $state->{"aim"}->buddy($sn);
	return $foo->{"screenname"} if (defined($foo));
	return $sn;
}

1;
