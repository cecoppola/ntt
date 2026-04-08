In this document, you will construct the master plan for the algorithmic choices and implementations of this project.

This document will consist of a sequence of segments, each one corresponding to a major segment of the code. So, the first task is to identify all of the segments of the number theoretic transform and order them, along with how the fraction of the total runtime each segment consumes, and which logic and memory it uses.

Within each section, you will describe the current, future, and possible choices for algorithms and implementations. In each section, you will construct a table, where each row in the table corresponds to one option for how to implement that segment. The columns of this table will include a description of the algorithm, difficulty of implementing correctly (total amount of work it will take you), total lines of code, impact on total algorithm runtime, memory usage of the algorithm, and any other metrics that are important for making these choices. Use the format in table_format.md.

As we progress, you will indicate which choices have been made in the table, and provide descriptions after each table about recommended options, improvements, and current status of that segment.

At the end of this document, you will give an overall assessment of the current status and performance of the code, which you will update after any major development.