#!/bin/bash
# Cancel GitLab CI pipelines (or manual jobs) without clicking each one in the UI.
#
#   export GITLAB_TOKEN='glpat-...'   # api scope
#   ./ci/cancel-manual-pipelines.sh --dry-run
#   ./ci/cancel-manual-pipelines.sh
#
# See: https://gitlab.com/lvmteam/lvm2/-/pipelines?scope=all&status=manual

set -euo pipefail

die() {
	echo "ERROR: $*" >&2
	exit 1
}

usage() {
	cat <<'EOF'
Usage: cancel-manual-pipelines.sh [options]

Cancel pipelines stuck in "manual" (or another status), or cancel manual jobs.

Options:
  --dry-run          List IDs only; do not cancel
  --project PATH     GitLab project path (default: from origin remote, else lvmteam/lvm2)
  --host URL         GitLab API host (default: https://gitlab.com)
  --status STATUS    Pipeline status filter (default: manual)
  --jobs             Cancel manual jobs instead of whole pipelines
  --per-page N       API page size (default: 100)
  -h, --help         Show this help

Environment:
  GITLAB_TOKEN       Personal access token with api scope (required)
EOF
}

gitlab_project_from_origin() {
	local url path
	url="$(git -C "${REPO_ROOT}" remote get-url origin 2>/dev/null)" || return 1
	case "$url" in
	git@*:*)
		path="${url#*:}"
		;;
	*://*/*)
		path="${url#*://*/}"
		;;
	*)
		return 1
		;;
	esac
	path="${path%.git}"
	[[ -n "$path" ]] || return 1
	printf '%s' "$path"
}

urlencode_project() {
	local p="${1//\//%2F}"
	printf '%s' "$p"
}

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
API_HOST="${GITLAB_HOST:-https://gitlab.com}"
PROJECT=""
PIPELINE_STATUS="manual"
JOBS=0
DRY_RUN=0
PER_PAGE=100

while [[ $# -gt 0 ]]; do
	case "$1" in
	--dry-run)
		DRY_RUN=1
		shift
		;;
	--project)
		[[ $# -ge 2 ]] || die "Missing value for --project"
		PROJECT="$2"
		shift 2
		;;
	--host)
		[[ $# -ge 2 ]] || die "Missing value for --host"
		API_HOST="$2"
		shift 2
		;;
	--status)
		[[ $# -ge 2 ]] || die "Missing value for --status"
		PIPELINE_STATUS="$2"
		shift 2
		;;
	--jobs)
		JOBS=1
		shift
		;;
	--per-page)
		[[ $# -ge 2 ]] || die "Missing value for --per-page"
		PER_PAGE="$2"
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	-*)
		die "Unknown option '$1' (try --help)"
		;;
	*)
		die "Unexpected argument '$1'"
		;;
	esac
done

command -v curl >/dev/null || die "curl is required"
command -v jq >/dev/null || die "jq is required"

if [[ -z "${GITLAB_TOKEN:-}" ]]; then
	die "Set GITLAB_TOKEN (personal access token with api scope)"
fi

if [[ -z "$PROJECT" ]]; then
	PROJECT="$(gitlab_project_from_origin)" || PROJECT="lvmteam/lvm2"
fi

ENC_PROJECT="$(urlencode_project "$PROJECT")"
API="${API_HOST%/}/api/v4"
AUTH=(--header "PRIVATE-TOKEN: ${GITLAB_TOKEN}")

cancel_pipeline() {
	local id="$1"
	if [[ "$DRY_RUN" -eq 1 ]]; then
		echo "pipeline $id"
		return 0
	fi
	curl -fsS "${AUTH[@]}" --request POST \
		"${API}/projects/${ENC_PROJECT}/pipelines/${id}/cancel" >/dev/null
	echo "canceled pipeline $id"
}

cancel_job() {
	local id="$1"
	if [[ "$DRY_RUN" -eq 1 ]]; then
		echo "job $id"
		return 0
	fi
	curl -fsS "${AUTH[@]}" --request POST \
		"${API}/projects/${ENC_PROJECT}/jobs/${id}/cancel" >/dev/null
	echo "canceled job $id"
}

fetch_page() {
	local url="$1"
	curl -fsS "${AUTH[@]}" "$url"
}

cancel_items() {
	local collection="$1"
	local filter="$2"
	local cancel_function="$3"
	local page=1
	local total=0
	local url
	local chunk
	local n
	local id

	while :; do
		url="${API}/projects/${ENC_PROJECT}/${collection}?${filter}&per_page=${PER_PAGE}&page=${page}"
		chunk="$(fetch_page "$url")"
		n="$(jq 'length' <<<"$chunk")"
		[[ "$n" -eq 0 ]] && break
		while read -r id; do
			[[ -n "$id" ]] || continue
			"$cancel_function" "$id"
			total=$((total + 1))
		done < <(jq -r '.[].id' <<<"$chunk")
		page=$((page + 1))
	done
	if [[ "$collection" == "jobs" ]]; then
		echo "Done: ${total} manual job(s) ($([[ "$DRY_RUN" -eq 1 ]] && echo dry-run || echo canceled))"
	else
		echo "Done: ${total} pipeline(s) with status=${PIPELINE_STATUS} ($([[ "$DRY_RUN" -eq 1 ]] && echo dry-run || echo canceled))"
	fi
}

if [[ "$JOBS" -eq 1 ]]; then
	cancel_items "jobs" "scope[]=manual" cancel_job
else
	cancel_items "pipelines" "status=${PIPELINE_STATUS}" cancel_pipeline
fi
