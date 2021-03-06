#! /usr/bin/perl -w
#-------------------------------------------------------------------------
#
# Gen_fmgrtab.pl
#    Perl equivalent of Gen_fmgrtab.sh
#
# Usage: perl Gen_fmgrtab.pl path-to-pg_proc.h
#
# The reason for implementing this functionality twice is that we don't
# require people to have perl to build from a tarball, but on the other
# hand Windows can't deal with shell scripts.
#
# Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/Gen_fmgrtab.pl,v 1.1 2008/06/23 17:54:29 tgl Exp $
#
#-------------------------------------------------------------------------

use strict;
use warnings;

# Collect arguments
my $infile = shift;
defined($infile) || die "$0: missing required argument: pg_proc.h\n";

# Note: see Gen_fmgrtab.sh for detailed commentary on what this is doing

# Collect column numbers for pg_proc columns we need
my ($proname, $prolang, $proisstrict, $proretset, $pronargs, $prosrc);

open(I, $infile) || die "Could not open $infile: $!";
while (<I>)
{
    if (m/#define Anum_pg_proc_proname\s+(\d+)/) {
	$proname = $1;
    }
    if (m/#define Anum_pg_proc_prolang\s+(\d+)/) {
	$prolang = $1;
    }
    if (m/#define Anum_pg_proc_proisstrict\s+(\d+)/) {
	$proisstrict = $1;
    }
    if (m/#define Anum_pg_proc_proretset\s+(\d+)/) {
	$proretset = $1;
    }
    if (m/#define Anum_pg_proc_pronargs\s+(\d+)/) {
	$pronargs = $1;
    }
    if (m/#define Anum_pg_proc_prosrc\s+(\d+)/) {
	$prosrc = $1;
    }
}
close(I);

# Collect the raw data
my @fmgr = ();

open(I, $infile) || die "Could not open $infile: $!";
while (<I>)
{
    next unless (/^DATA/);
    s/^[^O]*OID[^=]*=[ \t]*//;
    s/\(//;
    s/"[^"]*"/"xxx"/g;
    my @p = split;
    next if ($p[$prolang] ne "12");
    push @fmgr,
      {
        oid     => $p[0],
        proname => $p[$proname],
        strict  => $p[$proisstrict],
        retset  => $p[$proretset],
        nargs   => $p[$pronargs],
        prosrc  => $p[$prosrc],
      };
}
close(I);

# Emit headers for both files
open(H, '>', "$$-fmgroids.h") || die "Could not open $$-fmgroids.h: $!";
print H 
qq|/*-------------------------------------------------------------------------
 *
 * fmgroids.h
 *    Macros that define the OIDs of built-in functions.
 *
 * These macros can be used to avoid a catalog lookup when a specific
 * fmgr-callable function needs to be referenced.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTES
 *	******************************
 *	*** DO NOT EDIT THIS FILE! ***
 *	******************************
 *
 *	It has been GENERATED by $0
 *	from $infile
 *
 *-------------------------------------------------------------------------
 */
#ifndef FMGROIDS_H
#define FMGROIDS_H

/*
 *	Constant macros for the OIDs of entries in pg_proc.
 *
 *	NOTE: macros are named after the prosrc value, ie the actual C name
 *	of the implementing function, not the proname which may be overloaded.
 *	For example, we want to be able to assign different macro names to both
 *	char_text() and name_text() even though these both appear with proname
 *	'text'.  If the same C function appears in more than one pg_proc entry,
 *	its equivalent macro will be defined with the lowest OID among those
 *	entries.
 */
|;

open(T, '>', "$$-fmgrtab.c") || die "Could not open $$-fmgrtab.c: $!";
print T
qq|/*-------------------------------------------------------------------------
 *
 * fmgrtab.c
 *    The function manager's table of internal functions.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTES
 *
 *	******************************
 *	*** DO NOT EDIT THIS FILE! ***
 *	******************************
 *
 *	It has been GENERATED by $0
 *	from $infile
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/fmgrtab.h"

|;

# Emit #define's and extern's -- only one per prosrc value
my %seenit;
foreach my $s (sort {$a->{oid} <=> $b->{oid}} @fmgr)
{
    next if $seenit{$s->{prosrc}};
    $seenit{$s->{prosrc}} = 1;
    print H "#define F_" . uc $s->{prosrc} . " $s->{oid}\n";
    print T "extern Datum $s->{prosrc} (PG_FUNCTION_ARGS);\n";
}

# Create the fmgr_builtins table
print T "\nconst FmgrBuiltin fmgr_builtins[] = {\n";
my %bmap;
$bmap{'t'} = 'true';
$bmap{'f'} = 'false';
foreach my $s (sort {$a->{oid} <=> $b->{oid}} @fmgr)
{
    print T
	"  { $s->{oid}, \"$s->{prosrc}\", $s->{nargs}, $bmap{$s->{strict}}, $bmap{$s->{retset}}, $s->{prosrc} },\n";
}

# And add the file footers.
print H "\n#endif /* FMGROIDS_H */\n";
close(H);

print T
qq|  /* dummy entry is easier than getting rid of comma after last real one */
  /* (not that there has ever been anything wrong with *having* a
     comma after the last field in an array initializer) */
  { 0, NULL, 0, false, false, NULL }
};

/* Note fmgr_nbuiltins excludes the dummy entry */
const int fmgr_nbuiltins = (sizeof(fmgr_builtins) / sizeof(FmgrBuiltin)) - 1;
|;

close(T);

# Finally, rename the completed files into place.
rename "$$-fmgroids.h", "fmgroids.h"
    || die "Could not rename $$-fmgroids.h to fmgroids.h: $!";
rename "$$-fmgrtab.c", "fmgrtab.c"
    || die "Could not rename $$-fmgrtab.c to fmgrtab.c: $!";

exit 0;
