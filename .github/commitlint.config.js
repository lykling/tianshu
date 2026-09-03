// =============================================================================
// TIANSHU commitlint configuration
// =============================================================================
//
// Enforces Conventional Commits format on all commit messages.
// Per ADR-0009.

module.exports = {
    extends: ['@commitlint/config-conventional'],
    rules: {
        'type-enum': [
            2,
            'always',
            [
                'feat',
                'fix',
                'docs',
                'style',
                'refactor',
                'perf',
                'test',
                'build',
                'ci',
                'chore',
                'revert',
            ],
        ],
        // subject-case disabled: technical subjects legitimately start with
        // uppercase acronyms (ADR-0024, DSL, SLA, CLI, GPU...).
        'subject-case': [0],
        'subject-max-length': [2, 'always', 100],
        'subject-empty': [2, 'never'],
        'body-leading-blank': [1, 'always'],
        'footer-leading-blank': [1, 'always'],
    },
};
