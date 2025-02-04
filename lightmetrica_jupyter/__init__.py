"""IPython module for Lightmetrica"""
import os
import sys
import shutil
import filecmp
from tqdm import tqdm_notebook
import numpy as np
import matplotlib.pyplot as plt
import platform
import lightmetrica as lm
import time
from IPython.display import display, Markdown

def jupyter_init_config(outfilm):
    """ init() configuration for jupyter notebook extension """
    return {
        # Configure logger for jupyter notebook
        'logger': 'logger::jupyter',
        
        # Configure progress reporter for jupyter notebook
        # We can utilize multiple instance of progress reporters.
        # In this example, we used progress bar UI (progress::jupyter)
        # and progress image reporter (progress::film_jupyter)
        'progress': {
            'progress::mux': [
                {
                    'progress::jupyter': {}
                },
                {
                    'progress::delay': {
                        'delay': 1000,
                        'progress': {
                            'progress::film_jupyter': {
                                'film': outfilm,
                                'size': [10,5]
                            }
                        }
                    }
                }
            ]
        }
    }

def widen(arg):
    from IPython.core.display import display, HTML
    display(HTML("<style>.container { width:100% !important; }</style>"))

def load_ipython_extension(ip):
    """Register as IPython extension"""
    # Register line magic function
    ip.register_magic_function(widen)

    # Register some components
    @lm.pylm_component('logger::jupyter')
    class JupyterLogger(lm.log.LoggerContext):
        """Logger for jupyter notebook"""
        def construct(self, prop):
            self.severity = 0
            self.n = 0
            self.start = time.time()
            return True
        def log(self, level, severity, filename, line, message):
            if self.severity > severity:
                return
            out = sys.stdout
            if level == lm.log.LogLevel.Debug:
                name = 'D'
            elif level == lm.log.LogLevel.Info:
                name = 'I'
            elif level == lm.log.LogLevel.Warn:
                name = 'W'
                out = sys.stderr
            elif level == lm.log.LogLevel.Err:
                name = 'E'
                out = sys.stderr
            else:
                name = 'I'
            elapsed = time.time() - self.start
            file_no_ext = os.path.splitext(os.path.basename(filename))[0]
            line_and_file = '{}@{}'.format(line, file_no_ext)[:10]
            header = '[{}|{:.3f}|{:<10}] '.format(name, elapsed, line_and_file)
            spaces = ('.' * (self.n * 2)) + (' ' if self.n > 0 else '')
            s = header + spaces + message
            print(s, file=out)
        def updateIndentation(self, n):
            self.n += n
        def setSeverity(self, severity):
            self.severity = severity
        
    @lm.pylm_component('progress::jupyter')
    class JupyterProgress(lm.progress.ProgressContext):
        """Progress reporter for jupyter notebook"""
        def construct(self, prop):
            return True
        def start(self, mode, total, totalTime):
            self.mode = mode
            if mode == lm.progress.ProgressMode.Samples:
                self.pbar = tqdm_notebook(total=total)
            elif mode == lm.progress.ProgressMode.Time:
                self.pbar = tqdm_notebook(total=totalTime)
        def update(self, processed):
            self.pbar.update(max(0, processed - self.pbar.n))
        def updateTime(self, elapsed):
            self.pbar.update(max(0, elapsed - self.pbar.n))
        def end(self):
            self.pbar.update(max(0, self.pbar.total - self.pbar.n))
            self.pbar.close()

    @lm.pylm_component('progress::film_jupyter')
    class JupyterFilmProgress(lm.progress.ProgressContext):
        """Progress reporter for film"""
        def construct(self, prop):
            self.film = prop['film']
            self.size = prop['size']
            return True
        def start(self, mode, total, totalTime):
            self.total = total
            self.fig = plt.figure(figsize=self.size)
            self.ax = self.fig.add_subplot(111)
            plt.ion()
            plt.tight_layout()
            self.fig.show()
            self.fig.canvas.draw()
        def update(self, processed):
            self.ax.clear()
            img = np.flip(lm.buffer(self.film), axis=0)
            self.ax.imshow(np.clip(np.power(img,1/2.2),0,1))
            self.fig.canvas.draw()
        def updateTime(self, elapsed):
            self.update(-1)
        def end(self):
            self.update(self.total)
            import time
            time.sleep(1)
            plt.show()
            plt.close()
