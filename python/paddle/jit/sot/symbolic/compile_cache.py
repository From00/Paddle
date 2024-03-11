# Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import annotations

import inspect
from typing import TYPE_CHECKING

import paddle
from paddle.amp.auto_cast import amp_state
from paddle.base.data_feeder import convert_dtype
from paddle.framework import _dygraph_tracer, use_pir_api

from ..infer_meta import convert_meta_to_input_spec
from ..profiler import EventGuard
from ..utils import (
    ENV_SOT_EXPORT,
    Cache,
    GraphLogger,
    Singleton,
    StepInfoManager,
    log,
    log_do,
    map_if,
)
from .export import export
from .interpreter import compile_sir

if TYPE_CHECKING:
    from .symbolic_context import SymbolicTraceContext


def trace_back_frames():
    frame = inspect.currentframe()
    while frame.f_back is not None:
        frame = frame.f_back
        code = frame.f_code
        paddle.framework.core.sot_set_with_graph(code)


def clear_eager_tensor_name(output_tensors):
    for output_tensor in output_tensors:
        output_tensor.name = ""


class FallbackWrapper:
    """
    Used to store and call static graph methods generated by paddle.jit.to_static
    """

    def __init__(self, compiled_fn, SIR, is_training: bool):
        self.compiled_fn = compiled_fn
        self.partial_program = None
        self.concrete_program = None
        self.SIR = SIR  # for debug
        self.is_training = is_training
        self.exported = False

    def amp_cast_inputs(self, args, kwargs):
        """Prepare inputs for amp, cast float16 into float32 if needed."""
        current_amp_state = amp_state()
        if current_amp_state is None:
            return args, kwargs
        # skip if not gpu / xpu / custom place
        tracer = _dygraph_tracer()
        if not (
            tracer._expected_place.is_gpu_place()
            or tracer._expected_place.is_xpu_place()
            or tracer._expected_place.is_custom_place()
        ):
            return args, kwargs
        amp_dtype = convert_dtype(current_amp_state["dtype"])
        log(3, f"[AMP] Cast {amp_dtype} into float32\n")
        return map_if(
            (args, kwargs),
            pred=lambda x: isinstance(x, paddle.Tensor)
            and convert_dtype(x.dtype) == amp_dtype,
            true_fn=lambda x: x.cast(paddle.float32),
            false_fn=lambda x: x,
        )

    def graph_size(self):
        if self.partial_program is None:
            input_spec = convert_meta_to_input_spec(
                [self.SIR.symbol_meta_map[symbol] for symbol in self.SIR.inputs]
            )
            (
                self.concrete_program,
                self.partial_program,
            ) = self.compiled_fn.get_concrete_program(input_spec)
            self.partial_program.training = self.is_training
        if use_pir_api():
            return len(self.partial_program.program.program.global_block().ops)
        else:
            if self.partial_program.program.num_blocks > 1:
                return -1
            return len(self.partial_program.program.block(0).ops)

    def __call__(self, *args, **kwargs):
        with EventGuard(f"FallbackWrapper: {self.SIR.name}"):
            if StepInfoManager().need_back_trace:
                trace_back_frames()

            log_do(
                2,
                lambda: print("[FallbackWrapper] start run SIR: \n", self.SIR),
            )
            args, kwargs = self.amp_cast_inputs(args, kwargs)
            log_do(
                4,
                lambda: print(
                    self.compiled_fn.get_concrete_program(*args, **kwargs)[
                        1
                    ].train_program
                ),
            )
            if self.partial_program is None:
                with EventGuard("FallbackWrapper: get_concrete_program"):
                    (
                        self.concrete_program,
                        self.partial_program,
                    ) = self.compiled_fn.get_concrete_program(*args, **kwargs)
                    self.partial_program.training = self.is_training
            with EventGuard("FallbackWrapper: sot call partial_program"):
                outputs = self.partial_program.sot_call(*args, **kwargs)

            clear_eager_tensor_name(outputs)
            log_do(
                1,
                lambda: GraphLogger().add_subgraph(
                    self.concrete_program.main_program
                ),
            )
            log_do(
                4,
                lambda: print("[CompileCache] run sir forward success."),
            )
            if ENV_SOT_EXPORT.get() != "" and not self.exported:
                export(self.SIR, ENV_SOT_EXPORT.get())
                self.exported = True

            return outputs


@Singleton
class CompileSIRCache(Cache):
    """
    Cache the compiled function of SIR
    """

    def __init__(self):
        super().__init__(weak=False)

    def key_fn(self, context: SymbolicTraceContext, sir_name: str, **kwargs):
        """
        generate a hash key for a SIR

        Args:
            context: The context to compile
            sir_name: The name of the sir to compile
            build_strategy: The build strategy to compile

        Returns:
            The hash key of the SIR
        """
        sir = context.get_sir(sir_name)
        # NOTE(dev): Is str(sir) a heavy operation ?
        hash_key = hash((str(sir), kwargs['training']))
        return hash_key

    def value_fn(self, context: SymbolicTraceContext, sir_name: str, **kwargs):
        """
        Generate static graph function

        Args:
            context: The context to compile
            sir_name: The name of the sir to compile
            build_strategy: The build strategy to compile

        Returns:
            The static graph function
        """
        build_strategy = kwargs.get("build_strategy", None)
        backend = kwargs.get("backend", None)
        return FallbackWrapper(
            paddle.jit.to_static(
                compile_sir(context, sir_name),
                build_strategy=build_strategy,
                backend=backend,
                full_graph=True,
            ),
            context.get_sir(sir_name),
            is_training=kwargs['training'],
        )