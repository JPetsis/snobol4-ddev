<?php

namespace Snobol\Tests;

use PHPUnit\Framework\TestCase;

/**
 * @group coupling-probe
 *
 * Regression guard: the C probe and the PHP probe must both produce valid
 * output and stay within a loose ratio bound.
 *
 * The diagnostic-probe change established two complementary attribution
 * tools — a C-side probe that measures the engine's per-iteration cost
 * (bench/c/bench_probe.c) and a PHP-side probe that measures the full
 * user-facing cost (bench/php/probe.php). The PHP probe includes the
 * binding's overhead.
 *
 * This test runs both probes and asserts:
 *
 *   1. Both probes produce non-zero iterations for every scenario.
 *   2. Per-scenario PHP/C ratio bounds on ns-per-iteration: the anchored
 *      match rows of the residue family (residue_repeat, residue_zero_width,
 *      prefilter_miss, zero_progress) plus the original alt_literals check.
 *      Each bound is loose (≈10x) — failing anchored matches and
 *      result-array building legitimately cost more on the PHP side — but a
 *      PHP path falling back to a slower engine path while the C path stays
 *      on an accelerated tier pushes the ratio past the bound.
 */
class CPhpCouplingTest extends TestCase
{
    private const ITER_ENV = 'PROBE_ITERS';
    private const ITER_DEFAULT = 10000;   // C probe iterations (fast test)
    private const PHP_ITER_DEFAULT = 50000; // PHP probe: residue rows use iters/100,
                                            // so 50000 keeps them JIT-warm (~500 iters)

    /** @var array<string, float> scenario name => max PHP/C ratio */
    private const RATIO_BOUNDS = [
        'alt_literals'       => 50.0, // original loose check
        'residue_repeat'     => 10.0,
        'residue_zero_width' => 10.0,
        'prefilter_miss'     => 10.0,
        'zero_progress'      => 10.0,
    ];

    /** @return array<int, array<string, mixed>>|null */
    private function runPhpProbe(): ?array
    {
        $probe = $this->resolvePhpProbe();
        if ($probe === null) {
            $this->markTestSkipped('PHP probe not found');
            return null;
        }

        /* Set PROBE_ITERS in the environment so shell_exec() child inherits it,
         * and also pass it as a shell variable for robustness. The PHP probe
         * divides by 100 for the slow residue scenarios; 50000 keeps those at
         * ~500 iterations so the numbers are JIT-warm and stable. */
        putenv(self::ITER_ENV . '=' . self::PHP_ITER_DEFAULT);
        $cmd = sprintf(
            'PROBE_ITERS=%d %s %s',
            self::PHP_ITER_DEFAULT,
            escapeshellarg(PHP_BINARY),
            escapeshellarg($probe)
        );
        $out = shell_exec($cmd);
        if ($out === null) {
            $this->markTestSkipped('php probe execution failed');
            return null;
        }

        return $this->extractJsonResults($out);
    }

    /** @return array<int, array<string, mixed>>|null */
    private function runCProbe(): ?array
    {
        $probe = $this->resolveCProbe();
        if ($probe === null) {
            $this->markTestSkipped('C probe not built');
            return null;
        }

        putenv(self::ITER_ENV . '=' . self::ITER_DEFAULT);
        $cmd = sprintf(
            'PROBE_ITERS=%d %s',
            self::ITER_DEFAULT,
            escapeshellarg($probe)
        );
        $out = shell_exec($cmd);
        if ($out === null) {
            $this->markTestSkipped('c probe execution failed');
            return null;
        }

        return $this->parseCProbeTable((string)$out);
    }

    public function testBothProbesProduceValidOutput(): void
    {
        $php = $this->runPhpProbe();
        $c = $this->runCProbe();
        if ($php === null || $c === null) {
            return; // skipped
        }

        $this->assertNotEmpty($php, 'PHP probe produced no rows');
        $this->assertNotEmpty($c, 'C probe produced no rows');

        foreach ($php as $row) {
            $this->assertGreaterThan(0, $row['iters'],
                "PHP probe scenario '{$row['name']}' produced zero iters");
        }
        foreach ($c as $row) {
            $this->assertGreaterThan(0, $row['iters'],
                "C probe scenario '{$row['name']}' produced zero iters");
        }
    }

    public function testPhpC_ratio_within_bounds(): void
    {
        $php = $this->runPhpProbe();
        $c = $this->runCProbe();
        if ($php === null || $c === null) return;

        $checked = 0;
        foreach (self::RATIO_BOUNDS as $name => $max_ratio) {
            // Match by scenario name AND unit: the guarded rows are
            // anchored first-match (unit=match) on both probes.
            $c_row = $this->findRow($c, $name, 'match');
            $php_row = $this->findRow($php, $name, 'match');
            if ($c_row === null || $php_row === null) {
                // Per-row skip when a probe lacks the row (existing pattern).
                continue;
            }

            // Compare ns-per-iteration: the probes may use different
            // iteration counts for the same scenario (PHP runs the slow
            // residue rows at iters/100), so total_ns is not comparable —
            // the per-unit-of-work cost is.
            $c_ns = (int)$c_row['ns_per_iter'];
            $php_ns = (int)$php_row['ns_per_iter'];
            $this->assertGreaterThan(0, $c_ns, "C probe {$name} ns_per_iter is zero");
            $this->assertGreaterThan(0, $php_ns, "PHP probe {$name} ns_per_iter is zero");

            $ratio = $php_ns / $c_ns;
            $this->assertLessThanOrEqual(
                $max_ratio,
                $ratio,
                sprintf(
                    "PHP/C %s ratio %.1fx exceeds guard %.1fx\n"
                    . "  C:   %d ns/iter (iters=%d)\n"
                    . "  PHP: %d ns/iter (iters=%d)\n"
                    . "The binding is dominating.",
                    $name, $ratio, $max_ratio,
                    $c_ns, (int)$c_row['iters'],
                    $php_ns, (int)$php_row['iters']
                )
            );
            $checked++;
        }

        $this->assertGreaterThan(0, $checked, 'No comparable probe rows found');
    }

    // -----------------------------------------------------------------------
    // helpers
    // -----------------------------------------------------------------------

    /** @param array<int, array<string, mixed>> $rows */
    private function findRow(array $rows, string $name, ?string $unit = null): ?array
    {
        foreach ($rows as $r) {
            if ($r['name'] !== $name) {
                continue;
            }
            if ($unit !== null && ($r['unit'] ?? null) !== $unit) {
                continue;
            }
            return $r;
        }
        return null;
    }

    private function resolvePhpProbe(): ?string
    {
        $candidates = [
            __DIR__ . '/../../../bindings/php/probe.php', // actual PHP probe (repo)
            __DIR__ . '/../../../bench/php/probe.php',
            '/var/www/html/probe.php',                    // ddev (web root is bindings/php)
            '/var/www/bindings/php/probe.php',            // ddev (alt)
            '/var/www/bench/php/probe.php',               // ddev (legacy)
        ];
        foreach ($candidates as $p) {
            if (is_file($p)) return $p;
        }
        return null;
    }

    private function resolveCProbe(): ?string
    {
        $candidates = [
            '/usr/local/bin/snobol4_probe',                 // ddev installed
            __DIR__ . '/../../../build/bench/c/snobol4_probe',
            '/var/www/build/bench/c/snobol4_probe',         // ddev (html)
            '/var/www/project-root/build/bench/c/snobol4_probe', // ddev (project-root)
            __DIR__ . '/../../../../build/bench/c/snobol4_probe',
        ];
        foreach ($candidates as $p) {
            if (is_file($p) && is_executable($p)) return $p;
        }
        return null;
    }

    /**
     * The PHP probe now emits JSON at the end of stdout.
     *
     * @return array<int, array<string, mixed>>|null
     */
    private function extractJsonResults(string $out): ?array
    {
        $jsonStart = strrpos($out, '[');
        if ($jsonStart === false) return null;
        $json = substr($out, $jsonStart);
        $decoded = json_decode($json, true);
        return is_array($decoded) ? $decoded : null;
    }

    /**
     * Parse the C probe's table output.
     * Columns: scenario, ns/iter, iters.
     *
     * @return array<int, array<string, mixed>>
     */
    private function parseCProbeTable(string $out): array
    {
        $rows = [];
        $in_table = false;
        foreach (preg_split('/\r?\n/', $out) as $line) {
            if (preg_match('/^scenario\s+/', $line)) {
                $in_table = true;
                continue;
            }
            if (!$in_table) continue;
            if (preg_match('/^----+/', $line)) continue;
            if (preg_match('/^Legend:/', $line)) break;
            if (trim($line) === '') continue;

            // Format: name ns/iter iters [unit [tier exec]]
            $cols = preg_split('/\s+/', trim($line));
            if (count($cols) < 3) continue;
            $rows[] = [
                'name'        => $cols[0],
                'ns_per_iter' => (int)$cols[1],
                'iters'       => (int)$cols[2],
                'unit'        => $cols[3] ?? '',
                'total_ns'    => (int)$cols[1] * (int)$cols[2],
            ];
        }
        return $rows;
    }
}
