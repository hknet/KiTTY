#!/usr/bin/perl
# Generate a Halibut chapter for the manual from FEATURES.md, so the shipped
# help covers KiTTY's additions WITHOUT a second copy of the text to drift:
# FEATURES.md stays the single source, this runs at doc-build time
# (doc/CMakeLists.txt), the same pattern as cmake/licence.pl.
#
# Deliberate reductions, because Halibut is not Markdown:
#   - images are dropped (Halibut has no image support; the screenshots stay
#     in FEATURES.md / on the website),
#   - tables are rendered verbatim as code paragraphs,
#   - internal #anchor links keep their text and lose the link,
#   - the FEATURES.md table of contents block is skipped (the manual has its
#     own contents).
#
# Usage: features2but.pl <FEATURES.md> -o <kitty.but>

use strict;
use warnings;

my ($in, $out);
while (@ARGV) {
    my $a = shift @ARGV;
    if ($a eq '-o') { $out = shift @ARGV; }
    else { $in = $a; }
}
die "usage: features2but.pl <FEATURES.md> -o <kitty.but>\n"
    unless defined $in and defined $out;

open my $IN, '<', $in or die "$in: $!\n";
my @lines = <$IN>;
close $IN;

# Escape Halibut's specials, then convert the inline Markdown we actually use.
#
# Halibut cannot NEST text styles, so bold/italic conversion runs LAST and
# only wraps content that carries no style of its own: **`kitty.exe -x`**
# becomes plain \c{kitty.exe -x}, dropping the bold rather than emitting the
# illegal \e{\c{...}}.
sub styled {
    my ($s) = @_;
    return $s =~ /\\[ceW]\{/ ? $s : "\\e{$s}";
}
sub inline {
    my ($s) = @_;
    $s =~ s/\\/\\\\/g;
    $s =~ s/\{/\\{/g;
    $s =~ s/\}/\\}/g;
    # links first, while the brackets are still recognisable:
    # external -> \W{url}{text}; internal (#anchor) -> just the text.
    $s =~ s/!\[[^\]]*\]\([^)]*\)//g;                       # inline image: drop
    $s =~ s/\[([^\]]+)\]\((https?:[^)]+)\)/\\W{$2}{$1}/g;
    $s =~ s/\[([^\]]+)\]\([^)]*\)/$1/g;   # internal/relative link: keep text
    $s =~ s/`([^`]+)`/\\c{$1}/g;
    # non-greedy: bold text may contain a lone * (glob patterns like
    # **kittynew-*.sav**), which [^*]+ would refuse to cross.
    $s =~ s/\*\*(.+?)\*\*/styled($1)/ge;                   # bold -> emphasis
    $s =~ s/(?<![\w\\])\*([^*\s][^*]*)\*(?!\w)/styled($1)/ge;  # italic
    return $s;
}

# Section ids must be unique and Halibut-safe.
my %seen_id;
sub make_id {
    my ($t) = @_;
    $t = lc $t;
    $t =~ s/[^a-z0-9]+/-/g;
    $t =~ s/^-+|-+$//g;
    $t = "kitty-feat-$t";
    my $id = $t; my $n = 2;
    $id = $t . '-' . $n++ while $seen_id{$id}++;
    return $id;
}

open my $OUT, '>', $out or die "$out: $!\n";
print $OUT
    "\\# Generated from FEATURES.md by tools/features2but.pl - DO NOT EDIT.\n\n",
    "\\cfg{input-charset}{UTF-8}\n\n",
    "\\C{kitty-features} KiTTY additions\n\n",
    "This chapter covers what KiTTY adds on top of PuTTY. It is generated\n",
    "from the project's \\c{FEATURES.md}, which also carries the screenshots\n",
    "this manual format cannot include.\n\n";

my $i = 0;
my $intoc = 0;      # skipping FEATURES.md's own table of contents
my $started = 0;    # nothing is emitted before the first section heading
while ($i <= $#lines) {
    my $line = $lines[$i];
    chomp $line;

    # FEATURES.md's own TOC: from its marker to the first ## heading.
    if ($line =~ /^\*\*Table of contents\*\*/) { $intoc = 1; $i++; next; }
    if ($intoc) {
        if ($line =~ /^## /) { $intoc = 0; }
        else { $i++; next; }
    }

    if ($line =~ /^## (.*)$/) {
        my $t = $1;
        print $OUT "\\H{", make_id($t), "} ", inline($t), "\n\n";
        $started = 1;
    } elsif ($line =~ /^### (.*)$/) {
        my $t = $1;
        print $OUT "\\S{", make_id($t), "} ", inline($t), "\n\n";
        $started = 1;
    } elsif (!$started) {
        # the intro above the first heading is FEATURES.md's own blurb;
        # this chapter has its own.
    } elsif ($line =~ /^#### (.*)$/) {
        my $t = $1;
        print $OUT "\\S2{", make_id($t), "} ", inline($t), "\n\n";
    } elsif ($line =~ /^!\[/) {
        # image paragraph: dropped (see header comment)
    } elsif ($line =~ /^```/) {
        # fenced code block -> Halibut code paragraph, verbatim
        $i++;
        while ($i <= $#lines and $lines[$i] !~ /^```/) {
            my $c = $lines[$i]; chomp $c;
            print $OUT "\\c $c\n";
            $i++;
        }
        print $OUT "\n";
    } elsif ($line =~ /^\|/) {
        # table -> verbatim code paragraph (Halibut has no tables); the
        # verbatim form would show ** and backticks literally, so those are
        # stripped from the cells.
        while ($i <= $#lines and $lines[$i] =~ /^\|/) {
            my $c = $lines[$i]; chomp $c;
            $c =~ s/\*\*//g;
            $c =~ s/`//g;
            print $OUT "\\c $c\n";
            $i++;
        }
        print $OUT "\n";
        next;
    } elsif ($line =~ /^\s*[-*] (.*)$/) {
        # bullet item; continuation lines are folded into the same \b - and a
        # single blank line is looked PAST when what follows is an indented
        # continuation, or the item's own text gets cut mid-sentence.
        my $item = $1;
        while ($i + 1 <= $#lines) {
            if ($lines[$i+1] =~ /^\s{2,}(?![-*\s])(\S.*)$/) {
                $item .= ' ' . $1;
                $i++;
            } elsif ($lines[$i+1] =~ /^\s*$/ and $i + 2 <= $#lines and
                     $lines[$i+2] =~ /^\s{2,}(?![-*\s])\S/) {
                $i++;    # swallow the blank; the next loop pass folds the text
            } else {
                last;
            }
        }
        print $OUT "\\b ", inline($item), "\n\n";
    } elsif ($line =~ /^\s*$/) {
        # paragraph break: paragraphs below are emitted with their own blank line
    } else {
        # ordinary paragraph; fold following non-blank, non-special lines.
        my $para = $line;
        while ($i + 1 <= $#lines and $lines[$i+1] =~ /\S/
               and $lines[$i+1] !~ /^(##|###|####|!\[|```|\||\s*[-*] )/) {
            my $c = $lines[$i+1]; chomp $c;
            $para .= ' ' . $c;
            $i++;
        }
        print $OUT inline($para), "\n\n";
    }
    $i++;
}
close $OUT or die "$out: $!\n";
