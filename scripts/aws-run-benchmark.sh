#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
configured_region=${AWS_REGION:-$(aws configure get region || true)}
aws_region=${configured_region:-us-east-2}
stack_name=${NANOLOGGER_AWS_STACK_NAME:-nanologger-benchmark}
samples=${NANOLOGGER_BENCH_SAMPLES:-100000}
rate=${NANOLOGGER_BENCH_RATE:-100000}
keep_running=${NANOLOGGER_AWS_KEEP_RUNNING:-0}
suite=${NANOLOGGER_SUITE:-scripts/run-benchmark-suite.sh}
ab_patch=${NANOLOGGER_AB_PATCH:-shared-cursor.patch}
archive_file=$(mktemp -t nanologger-source.XXXXXX)
parameters_file=$(mktemp -t nanologger-ssm.XXXXXX)
readonly project_root aws_region stack_name samples rate keep_running suite ab_patch
readonly archive_file parameters_file
instance_id=""

aws_cli=(aws --region "$aws_region")
readonly -a aws_cli

fail() {
  echo "$1" >&2
  exit 1
}

cleanup() {
  local exit_code=$?
  trap - EXIT
  rm -f -- "$archive_file" "$parameters_file"
  if [[ "$keep_running" != "1" && -n "$instance_id" ]]; then
    echo "Stopping AWS benchmark VM $instance_id..."
    "${aws_cli[@]}" ec2 stop-instances --instance-ids "$instance_id" \
      --output json >/dev/null || true
    "${aws_cli[@]}" ec2 wait instance-stopped --instance-ids "$instance_id" || true
  fi
  exit "$exit_code"
}
trap cleanup EXIT

get_instance_id() {
  "${aws_cli[@]}" cloudformation describe-stacks \
    --stack-name "$stack_name" \
    --query 'Stacks[0].Outputs[?OutputKey==`InstanceId`].OutputValue | [0]' \
    --output text
}

ensure_instance_running() {
  local state
  state=$(
    "${aws_cli[@]}" ec2 describe-instances --instance-ids "$instance_id" \
      --query 'Reservations[0].Instances[0].State.Name' \
      --output text
  )

  case "$state" in
    stopped)
      echo "Starting AWS benchmark VM $instance_id..."
      "${aws_cli[@]}" ec2 start-instances --instance-ids "$instance_id" \
        --output json >/dev/null
      ;;
    running) ;;
    *) fail "Instance $instance_id is in unsupported state: $state" ;;
  esac

  "${aws_cli[@]}" ec2 wait instance-status-ok --instance-ids "$instance_id"
}

wait_for_ssm() {
  local attempt ping_status=""

  echo "Waiting for SSM agent..."
  for ((attempt = 1; attempt <= 60; attempt++)); do
    ping_status=$(
      "${aws_cli[@]}" ssm describe-instance-information \
        --filters Key=InstanceIds,Values="$instance_id" \
        --query 'InstanceInformationList[0].PingStatus' \
        --output text
    )
    [[ "$ping_status" == "Online" ]] && return
    sleep 5
  done

  fail "SSM agent did not become online for $instance_id"
}

create_source_archive() {
  tar --no-xattrs --exclude './build' --exclude './build-*' \
    --exclude '*.log' --exclude 'perf.data*' \
    -czf "$archive_file" -C "$project_root" .
}

write_remote_commands() {
  local payload
  payload=$(base64 < "$archive_file" | tr -d '\n')

  jq -n \
    --arg payload "$payload" \
    --arg samples "$samples" \
    --arg rate "$rate" \
    --arg suite "$suite" \
    --arg ab_patch "$ab_patch" \
    '{commands: [
      "set -euo pipefail",
      "cloud-init status --wait",
      "rm -rf /opt/nanologger/src",
      "mkdir -p /opt/nanologger/src /opt/nanologger/build",
      ("printf %s " + ($payload | @sh) + " | base64 -d | tar -xz -C /opt/nanologger/src"),
      "cd /opt/nanologger/src",
      ("NANOLOGGER_BUILD_DIR=/opt/nanologger/build NANOLOGGER_BENCH_SAMPLES=" + ($samples | @sh) + " NANOLOGGER_BENCH_RATE=" + ($rate | @sh) + " NANOLOGGER_AB_PATCH=" + ($ab_patch | @sh) + " bash " + ($suite | @sh))
    ]}' > "$parameters_file"
}

send_benchmark_command() {
  "${aws_cli[@]}" ssm send-command \
    --instance-ids "$instance_id" \
    --document-name AWS-RunShellScript \
    --comment "NanoLogger benchmark" \
    --timeout-seconds 1800 \
    --parameters "file://$parameters_file" \
    --query 'Command.CommandId' \
    --output text
}

wait_for_command() {
  local command_id=$1
  local status

  while true; do
    status=$(
      "${aws_cli[@]}" ssm get-command-invocation \
        --command-id "$command_id" \
        --instance-id "$instance_id" \
        --query Status \
        --output text 2>/dev/null || true
    )
    case "$status" in
      Success|Cancelled|TimedOut|Failed|Cancelling)
        printf '%s\n' "$status"
        return
        ;;
      *) sleep 5 ;;
    esac
  done
}

print_command_output() {
  local command_id=$1
  local stderr

  "${aws_cli[@]}" ssm get-command-invocation \
    --command-id "$command_id" \
    --instance-id "$instance_id" \
    --query StandardOutputContent \
    --output text
  stderr=$(
    "${aws_cli[@]}" ssm get-command-invocation \
      --command-id "$command_id" \
      --instance-id "$instance_id" \
      --query StandardErrorContent \
      --output text
  )
  [[ -z "$stderr" || "$stderr" == "None" ]] || printf '%s\n' "$stderr" >&2
}

get_response_code() {
  local command_id=$1
  "${aws_cli[@]}" ssm get-command-invocation \
    --command-id "$command_id" \
    --instance-id "$instance_id" \
    --query ResponseCode \
    --output text
}

instance_id=$(get_instance_id)
[[ -n "$instance_id" && "$instance_id" != "None" ]] || \
  fail "Stack $stack_name does not have an InstanceId output"

ensure_instance_running
wait_for_ssm
create_source_archive
write_remote_commands

echo "Running benchmark on $instance_id..."
command_id=$(send_benchmark_command)
status=$(wait_for_command "$command_id")
print_command_output "$command_id"
response_code=$(get_response_code "$command_id")

if [[ "$status" != "Success" || "$response_code" != "0" ]]; then
  fail "Remote benchmark failed: status=$status response_code=$response_code"
fi
