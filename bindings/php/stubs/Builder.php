<?php

/**
 * IDE stub for the Snobol\Builder class.
 *
 * The real implementation is provided by the libsnobol4 PHP extension
 * (bindings/php/src/snobol_builder_php.c). This file exists only so that IDEs
 * can resolve the class and its methods; it is never loaded at runtime because
 * the extension already registers the class at startup.
 *
 * Builder methods are static and fluent: each returns the builder node so calls
 * can be chained or passed into other builder methods.
 *
 * @generated from the extension method table — keep in sync with snobol_builder_php.c
 */

namespace Snobol;

class Builder
{
    /** @param mixed $arg @return static */
    public static function lit($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function span($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function brk($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function any($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function notany($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function len($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function arbno($arg): static { return new static(); }

    /** @param mixed $arg @return static Register in [0, 63]; ValueError outside */
    public static function cap($arg): static { return new static(); }

    /** @param mixed $name @param mixed $value @return static Registers in [0, 63]; ValueError outside */
    public static function assign($name, $value): static { return new static(); }

    /** @param mixed ...$parts @return static */
    public static function concat(...$parts): static { return new static(); }

    /** @param mixed ...$alts @return static */
    public static function alt(...$alts): static { return new static(); }

    /** @param mixed $arg @return static Register in [0, 63]; ValueError outside */
    public static function eval($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function anchor($arg): static { return new static(); }

    /** @param mixed $node @param int $min @param int|null $max @return static */
    public static function repeat($node, $min = 0, $max = null): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function emit($arg): static { return new static(); }

    /** @param mixed $arg @return static Register in [0, 63]; ValueError outside */
    public static function emitRef($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function label($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function goto($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function dynamicEval($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function tableAccess($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function tableUpdate($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function arrayAccess($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function arrayUpdate($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function arrayCreate($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function breakx($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function bal($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function fence($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function rem($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function rpos($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function rtab($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function arb($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function pos($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function tab($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function abort($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function fail($arg): static { return new static(); }

    /** @param mixed $arg @return static */
    public static function succeed($arg): static { return new static(); }
}
