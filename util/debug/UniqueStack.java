import java.util.HashMap;
import java.util.Vector;

public class UniqueStack
{
	static private HashMap stacks = new HashMap();
	
	private final Vector backtrace;
	private final int traceHash;
	
	private UniqueStack(StackTemplate data)
	{
		backtrace = data.backtrace;
		traceHash = data.traceHash;
		
		data.backtrace = null;
		data.traceHash = 0;
	}
	
	public int getFrameCount()
	{
		if(backtrace == null)
			return 0;
		return backtrace.size();
	}
	
	public int getStackFrame(int frame)
	{
		if(backtrace == null)
			return 0;
		return ((Integer) backtrace.get(frame)).intValue();
	}
	
	private boolean sameStack(StackTemplate data)
	{
		if(traceHash != data.traceHash)
			return false;
		if(backtrace == null && data.backtrace == null)
			return true;
		if(backtrace == null || data.backtrace == null)
			return false;
		int size = data.backtrace.size();
		if(backtrace.size() != size)
			return false;
		for(int i = 0; i < size; i++)
			if(!backtrace.get(i).equals(data.backtrace.get(i)))
				return false;
		return true;
	}
	
	public static class StackTemplate
	{
		private Vector backtrace;
		private int traceHash;
		
		public void addStackFrame(int address)
		{
			if(backtrace == null)
			{
				backtrace = new Vector();
				traceHash = 0x5AFEDA7A;
			}
			backtrace.add(new Integer(address));
			traceHash *= 37;
			traceHash ^= address;
		}
		
		public UniqueStack getUniqueStack()
		{
			Integer hash = new Integer(traceHash);
			Vector stack = (Vector) stacks.get(hash);
			UniqueStack test;
			
			if(stack != null)
			{
				int size = stack.size();
				for(int i = 0; i < size; i++)
				{
					test = (UniqueStack) stack.get(i);
					if(test.sameStack(this))
					{
						backtrace = null;
						traceHash = 0;
						return test;
					}
				}
			}
			else
			{
				stack = new Vector();
				stacks.put(hash, stack);
			}
			
			test = new UniqueStack(this);
			stack.add(test);
			return test;
		}
	}
}
