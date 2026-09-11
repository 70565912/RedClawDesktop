@{
    IncludeDefaultRules = $true
    ExcludeRules = @(
        # Operational scripts intentionally write explicit operator-facing output.
        'PSAvoidUsingWriteHost',
        # Existing repository scripts have mixed indentation/whitespace style and are not normalized yet.
        'PSUseConsistentIndentation',
        'PSUseConsistentWhitespace',
        # Legacy helper function names are retained to avoid churn in operational scripts.
        'PSUseSingularNouns'
    )

    Rules = @{
        PSUseConsistentWhitespace = @{
            Enable = $true
            CheckOpenBrace = $true
            CheckOpenParen = $true
            CheckOperator = $true
            CheckSeparator = $true
        }
        PSUseConsistentIndentation = @{
            Enable = $true
            Kind = 'space'
            IndentationSize = 4
            PipelineIndentation = 'IncreaseIndentationForFirstPipeline'
        }
        PSAlignAssignmentStatement = @{
            Enable = $false
        }
    }
}
