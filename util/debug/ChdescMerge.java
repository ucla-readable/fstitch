import java.io.DataInput;
import java.io.IOException;

public class ChdescMerge extends Opcode
{
	private final int count, chdescs, head, tail;
	
	public ChdescMerge(int count, int chdescs, int head, int tail)
	{
		this.count = count;
		this.chdescs = chdescs;
		this.head = head;
		this.tail = tail;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public boolean hasEffect()
	{
		return false;
	}
	
	public String toString()
	{
		return "KDB_CHDESC_MERGE: count = " + count + ", chdescs = " + SystemState.hex(chdescs) + ", head = " + SystemState.hex(head) + ", tail = " + SystemState.hex(tail);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_MERGE, "KDB_CHDESC_MERGE", ChdescMerge.class);
		factory.addParameter("count", 4);
		factory.addParameter("chdescs", 4);
		factory.addParameter("head", 4);
		factory.addParameter("tail", 4);
		return factory;
	}
}
