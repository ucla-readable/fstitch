import java.io.DataInput;
import java.io.IOException;

public class ChdescOverlapAttach extends Opcode
{
	private final int recent, original;
	
	public ChdescOverlapAttach(int recent, int original)
	{
		this.recent = recent;
		this.original = original;
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
		return "KDB_CHDESC_OVERLAP_ATTACH: recent = " + SystemState.hex(recent) + ", original = " + SystemState.hex(original);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_OVERLAP_ATTACH, "KDB_CHDESC_OVERLAP_ATTACH", ChdescOverlapAttach.class);
		factory.addParameter("recent", 4);
		factory.addParameter("original", 4);
		return factory;
	}
}
