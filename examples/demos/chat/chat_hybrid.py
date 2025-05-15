import sys
from threading import Thread, Event
from transformers import StoppingCriteriaList
from lemonade.tools.server.serve import StopOnEvent
from lemonade.api import from_pretrained
from lemonade.tools.ort_genai.oga import OrtGenaiStreamer


def main():

    model, tokenizer = from_pretrained(
        "amd/Llama-3.2-1B-Instruct-awq-g128-int4-asym-fp16-onnx-hybrid",
        recipe="oga-hybrid",
    )

    while True:
        # Enable sending a signal into the generator thread to stop
        # the generation early
        stop_event = Event()
        stopping_criteria = StoppingCriteriaList([StopOnEvent(stop_event)])

        # Prompt the user for an input message
        print()
        user_message = input("User: ")
        print()

        # Print a friendly message when we quit
        if user_message == "quit":
            print("System: Ok, bye!\n")
            break

        # Generate the response in a thread and stream the result back
        # to the main thread
        input_ids = tokenizer(user_message, return_tensors="pt").input_ids

        streamer = OrtGenaiStreamer(tokenizer)
        generation_kwargs = {
            "input_ids": input_ids,
            "streamer": streamer,
            "max_new_tokens": 200,
            "stopping_criteria": stopping_criteria,
        }

        thread = Thread(target=model.generate, kwargs=generation_kwargs)
        thread.start()

        # Print each word to the screen as it arrives from the streamer
        # Allow the user to terminate the response with
        # a keyboard interrupt (ctrl+c)
        try:
            print("LLM: ", end="")
            for new_text in streamer:
                print(new_text, end="")
                sys.stdout.flush()

        except KeyboardInterrupt:
            stop_event.set()

        print()

        thread.join()


if __name__ == "__main__":
    main()
